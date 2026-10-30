// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_sampler.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(_MSC_VER) && defined(_M_X64)
#define NX2_SAMPLER_SUPPORTED 1
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#endif

namespace Nx2Sampler {
namespace {

bool EnvSeconds(const char* name, double fallback, double& value) {
    const char* text = std::getenv(name);
    if (text == nullptr) {
        value = fallback;
        return true;
    }
    size_t length = 0;
    while (length <= 64 && text[length] != '\0') {
        ++length;
    }
    if (length > 64) {
        return false;
    }
    char* end{};
    errno = 0;
    value = std::strtod(text, &end);
    return end != text && *end == '\0' && errno != ERANGE && std::isfinite(value);
}

std::string Validate(const Options& options) {
    if (!std::isfinite(options.delay_seconds) || options.delay_seconds < 0.0 ||
        options.delay_seconds > MaxDelaySeconds) {
        return "delay must be finite and between 0 and 600 seconds";
    }
    if (!std::isfinite(options.duration_seconds) || options.duration_seconds <= 0.0 ||
        options.duration_seconds > MaxDurationSeconds) {
        return "duration must be finite, greater than 0 and at most 120 seconds";
    }
    if (options.output_path.empty() || options.output_path.size() > MaxOutputPathLength ||
        options.output_path.find('\0') != std::string::npos) {
        return "output path must contain 1..4096 bytes without embedded NUL";
    }
    if (options.patterns.empty() || options.patterns.size() > MaxPatterns) {
        return "supply 1..32 thread name patterns";
    }
    for (const auto& pattern : options.patterns) {
        if (pattern.empty() || pattern.size() > MaxPatternLength ||
            pattern.find('\0') != std::string::npos ||
            (pattern.find('*') != std::string::npos && pattern.find('*') != pattern.size() - 1)) {
            return "patterns must contain 1..255 bytes with only an optional trailing '*'";
        }
    }
    return {};
}

#ifdef NX2_SAMPLER_SUPPORTED
constexpr size_t MaxDepth = 40;

struct Sample {
    unsigned thread;
    unsigned depth;
    std::array<DWORD64, MaxDepth> frames;
};

struct Target {
    HANDLE handle;
    std::string name;

    Target(HANDLE handle_, std::string name_) : handle{handle_}, name{std::move(name_)} {}
    Target(Target&& other) noexcept : handle{other.handle}, name{std::move(other.name)} {
        other.handle = nullptr;
    }
    ~Target() {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
};

struct HandleCloser {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using OwnedHandle = std::unique_ptr<void, HandleCloser>;

bool Matches(const std::string& thread_name, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (!pattern.empty() && pattern.back() == '*') {
            if (thread_name.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0) {
                return true;
            }
        } else if (thread_name == pattern) {
            return true;
        }
    }
    return false;
}

std::vector<Target> FindThreads(const std::vector<std::string>& patterns, DWORD owner) {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    std::vector<Target> targets;
    targets.reserve(MaxTargets);
    if (get_description == nullptr) {
        throw std::runtime_error("GetThreadDescription is unavailable");
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("CreateToolhelp32Snapshot failed");
    }
    const OwnedHandle snapshot_owner{snapshot};
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self ||
            entry.th32ThreadID == owner) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (handle == nullptr) {
            continue;
        }
        OwnedHandle handle_owner{handle};
        PWSTR wide{};
        std::string name;
        if (SUCCEEDED(get_description(handle, &wide)) && wide != nullptr) {
            char buffer[256]{};
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, sizeof(buffer) - 1, nullptr,
                                nullptr);
            LocalFree(wide);
            name = buffer;
        }
        if (Matches(name, patterns)) {
            if (targets.size() == MaxTargets) {
                throw std::runtime_error("more than 16 target threads match; narrow the patterns");
            }
            targets.emplace_back(handle, std::move(name));
            handle_owner.release();
        }
    }
    return targets;
}

// Walks a suspended thread's stack from its context. Nothing here may allocate: the thread
// could be paused holding the heap lock.
unsigned Walk(CONTEXT context, std::array<DWORD64, MaxDepth>& frames) {
    unsigned depth = 0;
    while (depth < MaxDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf function (no unwind data): its return address is on top of the stack.
            // Registered Dynarmic JIT tables use the normal unwind path below.
            // Past the first frame, stop if no unwind metadata exists.
            if (depth != 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
    }
    return depth;
}

// No C++ objects with destructors, allocations, locks, or logging while suspended. SEH
// contains invalid unwind/stack memory and resumes the thread before reporting the error.
bool Capture(HANDLE handle, Sample& sample, DWORD& error) {
    if (SuspendThread(handle) == static_cast<DWORD>(-1)) {
        error = GetLastError();
        return false;
    }
    bool captured = false;
    __try {
        __try {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(handle, &context)) {
                sample.depth = Walk(context, sample.frames);
                captured = true;
            } else {
                error = GetLastError();
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            error = GetExceptionCode();
        }
    } __finally {
        if (ResumeThread(handle) == static_cast<DWORD>(-1)) {
            error = GetLastError();
            captured = false;
        }
    }
    return captured;
}

struct Symbolizer {
    using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
    using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymCleanupFn = BOOL(WINAPI*)(HANDLE);

    Symbolizer() {
        dbghelp = LoadLibraryExA("dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dbghelp == nullptr) {
            return;
        }
        const auto set_options =
            reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions"));
        const auto initialize =
            reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize"));
        from_address = reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
        cleanup = reinterpret_cast<SymCleanupFn>(GetProcAddress(dbghelp, "SymCleanup"));
        if (set_options == nullptr || initialize == nullptr || from_address == nullptr ||
            cleanup == nullptr) {
            from_address = nullptr;
            return;
        }
        set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (!initialize(GetCurrentProcess(), nullptr, TRUE)) {
            from_address = nullptr;
        }
    }

    ~Symbolizer() {
        if (from_address != nullptr) {
            cleanup(GetCurrentProcess());
        }
        if (dbghelp != nullptr) {
            FreeLibrary(dbghelp);
        }
    }

    const std::string& Name(DWORD64 address) {
        if (cache.size() >= 65536 && cache.find(address) == cache.end()) {
            throw std::runtime_error("report exceeds 65536 unique symbol addresses");
        }
        const auto [it, is_new] = cache.try_emplace(address);
        if (!is_new) {
            return it->second;
        }
        if (from_address != nullptr) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024]{};
            auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 1024;
            DWORD64 displacement{};
            if (from_address(GetCurrentProcess(), address, &displacement, symbol)) {
                it->second.assign(symbol->Name, std::min<size_t>(symbol->NameLen, 160));
                return it->second;
            }
        }
        HMODULE module{};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module)) {
            char path[MAX_PATH] = "?";
            GetModuleFileNameA(module, path, sizeof(path));
            const char* base = std::strrchr(path, '\\');
            it->second = std::string{base != nullptr ? base + 1 : path} + "!?";
        } else {
            it->second = "(jit)";
        }
        return it->second;
    }

    SymFromAddrFn from_address{};
    SymCleanupFn cleanup{};
    HMODULE dbghelp{};
    std::unordered_map<DWORD64, std::string> cache;
};

bool Report(const std::vector<Target>& targets, const std::vector<Sample>& samples,
            const char* path, std::stop_token token) {
    if (token.stop_requested()) {
        return false;
    }
    Symbolizer symbolizer;
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    File report{std::fopen(path, "w"), &std::fclose};
    if (!report) {
        throw std::runtime_error("cannot open report output path");
    }
    std::FILE* const file = report.get();
    for (unsigned thread = 0; thread < targets.size(); ++thread) {
        std::map<std::string, unsigned> self;
        std::map<std::string, unsigned> inclusive;
        std::map<std::string, unsigned> self_caller;
        unsigned total = 0;
        for (const Sample& sample : samples) {
            if (token.stop_requested()) {
                return false;
            }
            if (sample.thread != thread || sample.depth == 0) {
                continue;
            }
            ++total;
            const std::string& top = symbolizer.Name(sample.frames[0]);
            ++self[top];
            if (sample.depth > 1) {
                ++self_caller[top + "  <-  " + symbolizer.Name(sample.frames[1])];
            }
            std::set<std::string> seen;
            for (unsigned i = 0; i < sample.depth; ++i) {
                const std::string& name = symbolizer.Name(sample.frames[i]);
                if (seen.insert(name).second) {
                    ++inclusive[name];
                }
            }
        }
        const auto print = [&](const char* title, const std::map<std::string, unsigned>& counts,
                               size_t limit) {
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [name, count] : counts) {
                sorted.emplace_back(count, name);
            }
            std::sort(sorted.rbegin(), sorted.rend());
            std::fprintf(file, "-- %s\n", title);
            for (size_t i = 0; i < (std::min)(limit, sorted.size()); ++i) {
                std::fprintf(file, "%6u %5.1f%% %s\n", sorted[i].first,
                             100.0 * sorted[i].first / (std::max)(total, 1U),
                             sorted[i].second.c_str());
            }
        };
        std::fprintf(file, "=== thread \"%s\": %u samples\n", targets[thread].name.c_str(), total);
        print("self (where the thread is)", self, 60);
        print("self <- caller", self_caller, 50);
        print("inclusive (on the stack)", inclusive, 90);
        std::fprintf(file, "\n");
    }
    const bool report_error = std::ferror(file) != 0;
    const int report_close = std::fclose(report.release());
    if (report_error || report_close != 0) {
        throw std::runtime_error("writing report output failed");
    }

    // <out>.folded: every distinct stack with its count, root first ("thread;a;b;c count").
    std::map<std::string, unsigned> folded;
    size_t key_bytes = 0;
    for (const Sample& sample : samples) {
        if (token.stop_requested()) {
            return false;
        }
        std::string line = targets[sample.thread].name;
        for (unsigned i = sample.depth; i-- > 0;) {
            line += ';';
            line += symbolizer.Name(sample.frames[i]);
        }
        const auto [it, inserted] = folded.try_emplace(std::move(line), 0);
        if (inserted) {
            key_bytes += it->first.size();
            if (key_bytes > 32 * 1024 * 1024) {
                throw std::runtime_error("folded report exceeds 32 MiB of distinct stack keys");
            }
        }
        ++it->second;
    }
    File folded_file{std::fopen((std::string{path} + ".folded").c_str(), "w"), &std::fclose};
    if (!folded_file) {
        throw std::runtime_error("cannot open folded report output path");
    }
    for (const auto& [line, count] : folded) {
        if (token.stop_requested()) {
            return false;
        }
        std::fprintf(folded_file.get(), "%s %u\n", line.c_str(), count);
    }
    const bool folded_error = std::ferror(folded_file.get()) != 0;
    const int folded_close = std::fclose(folded_file.release());
    if (folded_error || folded_close != 0) {
        throw std::runtime_error("writing folded report output failed");
    }
    return true;
}
#endif // NX2_SAMPLER_SUPPORTED

} // Anonymous namespace

struct Session::Impl {
    // Lifecycle calls never hold status_mutex while joining. The worker never takes lifecycle_mutex.
    std::mutex lifecycle_mutex;
    mutable std::mutex status_mutex;
    Status status;
    bool attempted = false;
    bool stopped = false;
    std::jthread worker;
#ifdef NX2_SAMPLER_SUPPORTED
    const DWORD owner = GetCurrentThreadId();
#endif

    Status Snapshot() const {
        std::scoped_lock lock{status_mutex};
        return status;
    }

    void Publish(State state, size_t targets = 0, size_t samples = 0, std::string error = {}) {
        std::scoped_lock lock{status_mutex};
        status = {state, targets, samples, std::move(error)};
    }

    Status Start(Options options, std::string error = {}, bool disabled = false) {
        std::scoped_lock lock{lifecycle_mutex};
        if (attempted || stopped) {
            auto result = Snapshot();
            result.state = State::Failed;
            result.error = "session already started or stopped; create a new session";
            return result; // A rejected repeat does not overwrite the live worker's status.
        }
        attempted = true;
        if (disabled) {
            return Snapshot();
        }
        if (error.empty()) {
            error = Validate(options);
        }
#ifndef NX2_SAMPLER_SUPPORTED
        if (error.empty()) {
            error = "sampling is supported only on Windows x64 with MSVC";
        }
#endif
        if (!error.empty()) {
            Publish(State::Failed, 0, 0, std::move(error));
            return Snapshot();
        }
#ifdef NX2_SAMPLER_SUPPORTED
        Publish(State::Waiting);
        try {
            worker = std::jthread{[this, options = std::move(options)](std::stop_token token) {
                size_t target_count = 0;
                size_t sample_count = 0;
                try {
                    std::mutex wait_mutex;
                    std::condition_variable_any wake;
                    const auto wait = [&](auto duration) {
                        std::unique_lock wait_lock{wait_mutex};
                        wake.wait_for(wait_lock, token, duration, [] { return false; });
                        return token.stop_requested();
                    };
                    if (wait(std::chrono::duration<double>(options.delay_seconds))) {
                        Publish(State::Cancelled);
                        return;
                    }
                    const auto targets = FindThreads(options.patterns, owner);
                    target_count = targets.size();
                    if (targets.empty()) {
                        throw std::runtime_error("no eligible threads match; check host thread names");
                    }
                    std::vector<Sample> samples;
                    const size_t capacity =
                        static_cast<size_t>(std::ceil(options.duration_seconds * 1100.0)) *
                            targets.size() + 16;
                    if (capacity > MaxSamples) {
                        throw std::runtime_error(
                            "duration/target count exceeds the 262144-sample budget; narrow the patterns or duration");
                    }
                    samples.reserve(capacity);
                    Publish(State::Sampling, target_count);
                    std::fprintf(stderr, "nx2 sampler: %zu threads match, sampling %.0f s\n",
                                 target_count, options.duration_seconds);
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::duration<double>(options.duration_seconds);
                    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                        for (unsigned i = 0; i < targets.size(); ++i) {
                            if (token.stop_requested()) {
                                break;
                            }
                            if (samples.size() == capacity) {
                                throw std::runtime_error(
                                    "sample buffer limit reached; reduce duration or targets");
                            }
                            Sample sample{};
                            sample.thread = i;
                            DWORD error{};
                            const bool captured = Capture(targets[i].handle, sample, error);
                            // Target has been resumed before any allocation, status lock, or exception.
                            if (!captured) {
                                throw std::runtime_error(
                                    "thread capture/resume failed (Windows code " +
                                    std::to_string(error) + ")");
                            }
                            samples.push_back(sample);
                            sample_count = samples.size();
                        }
                        Publish(State::Sampling, target_count, sample_count);
                        if (wait(std::chrono::milliseconds(1))) {
                            break;
                        }
                    }
                    if (token.stop_requested()) {
                        Publish(State::Cancelled, target_count, sample_count);
                        return;
                    }
                    Publish(State::Reporting, target_count, sample_count);
                    if (!Report(targets, samples, options.output_path.c_str(), token)) {
                        Publish(State::Cancelled, target_count, sample_count);
                        return;
                    }
                    std::fprintf(stderr, "nx2 sampler: %zu samples written to %s\n", sample_count,
                                 options.output_path.c_str());
                    Publish(State::Completed, target_count, sample_count);
                } catch (const std::exception& exception) {
                    Publish(State::Failed, target_count, sample_count, exception.what());
                } catch (...) {
                    Publish(State::Failed, target_count, sample_count, "unexpected sampler failure");
                }
            }};
        } catch (const std::exception& exception) {
            Publish(State::Failed, 0, 0, exception.what());
        }
#endif
        return Snapshot();
    }
};

Session::Session() : impl{std::make_unique<Impl>()} {}

Session::~Session() {
    Stop();
}

Status Session::Start(Options options) {
    return impl->Start(std::move(options));
}

Status Session::StartFromEnvironment() {
    const char* names = std::getenv("EDEN_NX2_SAMPLE");
    if (names == nullptr || *names == '\0') {
        return impl->Start({}, {}, true);
    }
    Options options;
    // Bound parsing before building strings/vectors from environment-controlled input.
    const size_t max_names = MaxPatterns * (MaxPatternLength + 1);
    size_t length = 0;
    while (length <= max_names && names[length] != '\0') {
        ++length;
    }
    if (length > max_names) {
        return impl->Start({}, "EDEN_NX2_SAMPLE exceeds the pattern size limit");
    }
    const std::string names_text{names, length};
    for (size_t begin = 0;;) {
        const size_t comma = names_text.find(',', begin);
        options.patterns.push_back(names_text.substr(begin, comma - begin));
        if (options.patterns.size() > MaxPatterns) {
            return impl->Start({}, "supply at most 32 thread name patterns");
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    if (!EnvSeconds("EDEN_NX2_SAMPLE_START", 60.0, options.delay_seconds) ||
        !EnvSeconds("EDEN_NX2_SAMPLE_SECONDS", 30.0, options.duration_seconds)) {
        return impl->Start({}, "EDEN_NX2_SAMPLE_START/SECONDS must be finite numeric seconds");
    }
    const char* out = std::getenv("EDEN_NX2_SAMPLE_OUT");
    if (out != nullptr) {
        size_t out_length = 0;
        while (out_length <= MaxOutputPathLength && out[out_length] != '\0') {
            ++out_length;
        }
        if (out_length > MaxOutputPathLength) {
            return impl->Start({}, "EDEN_NX2_SAMPLE_OUT exceeds 4096 bytes");
        }
        options.output_path.assign(out, out_length);
    }
    return Start(std::move(options));
}

Status Session::GetStatus() const {
    return impl->Snapshot();
}

void Session::Stop() {
    std::scoped_lock lock{impl->lifecycle_mutex};
    impl->stopped = true;
    if (impl->worker.joinable()) {
        impl->worker.request_stop();
        impl->worker.join();
    }
}

} // namespace Nx2Sampler
