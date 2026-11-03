// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <set>
#include <unordered_map>
#include "core/debugger/guest_trace.h"
#include "core/debugger/trace_expression.h"

namespace Core {
namespace {
bool InstructionAddress(u64 address) {
    return address && !(address % 4) && address <= std::numeric_limits<u64>::max() - 4;
}
u32 CaptureSize(const GuestTrace::CaptureSpec& capture) {
    if (capture.type == "bytes" || capture.type == "c-string") return capture.size;
    if (capture.type == "u8" || capture.type == "bool") return 1;
    if (capture.type == "u16") return 2;
    if (capture.type == "u32" || capture.type == "i32" || capture.type == "f32") return 4;
    if (capture.type == "u64") return 8;
    return 0;
}
u64 Little64(const u8* p) {
    u64 value{};
    for (size_t i = 0; i < 8; ++i) value |= u64{p[i]} << (i * 8);
    return value;
}
} // namespace

struct GuestTrace::Impl {
    struct Entry {
        Point point;
        std::vector<TraceExpressions::Expression> expressions;
        std::optional<TraceExpressions::Condition> condition;
        Clock::time_point window{};
        u32 window_records{};
    };
    struct Return {
        u64 point_id, process_id, thread_id, address, sp;
    };
    mutable std::mutex mutex;
    std::vector<Entry> points;
    std::vector<Return> returns;
    std::deque<Record> records;
    u64 next_id{1}, sequence{}, evicted{}, process_id{}, main_thread{}, revision{};
    bool resolved{};
    std::set<u64> invalidations;
    bool invalidate_all{};

    bool HasWork() const {
        return !returns.empty() || std::ranges::any_of(points, [](const auto& entry) {
            return entry.point.state == State::Armed || entry.point.state == State::Pending;
        });
    }

    void RecordHit(Entry& entry, u64 thread_id, u32 core_id, u64 frame,
                   const Registers& registers, const MemoryReader& read,
                   Clock::time_point now, bool returning, const Describer& describe) {
        auto& point = entry.point;
        if (point.config.counters_only) return;
        if (now - entry.window >= std::chrono::seconds{1}) {
            entry.window = now;
            entry.window_records = 0;
        }
        if (entry.window_records >= point.config.rate_limit) {
            ++point.dropped;
            return;
        }
        ++entry.window_records;
        Record record;
        record.sequence = ++sequence;
        record.point_id = point.id;
        record.process_id = point.process_id;
        record.thread_id = thread_id;
        record.core_id = core_id;
        record.frame = frame;
        record.wall_time_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        record.returning = returning;
        for (const auto index : point.config.registers) {
            record.registers.emplace_back(index, registers[index]);
        }
        // A return always records x0 even if it wasn't selected at entry.
        if (returning && std::ranges::find(point.config.registers, 0) == point.config.registers.end()) {
            record.registers.emplace_back(u8{0}, registers[0]);
        }
        for (size_t i = 0; i < point.config.captures.size(); ++i) {
            Capture capture{point.config.captures[i]};
            const auto size = CaptureSize(capture.spec);
            if (!entry.expressions[i].Evaluate(registers, read, capture.address) ||
                capture.address > std::numeric_limits<u64>::max() - size) {
                capture.error = "address_unavailable";
            } else {
                if (capture.spec.type == "c-string") {
                    for (u32 offset = 0; offset < size; ++offset) {
                        std::array<u8, 1> byte{};
                        if (!read(capture.address + offset, byte)) {
                            capture.bytes.clear();
                            capture.error = "memory_unavailable";
                            break;
                        }
                        capture.bytes.push_back(byte[0]);
                        if (!byte[0]) break;
                    }
                } else {
                    capture.bytes.resize(size);
                    if (!read(capture.address, capture.bytes)) {
                        capture.bytes.clear();
                        capture.error = "memory_unavailable";
                    }
                }
            }
            record.captures.push_back(std::move(capture));
        }
        if (point.config.backtrace) {
            record.backtrace = {registers[32], registers[30]};
            u64 fp = registers[29];
            for (size_t i = 0; i < 14; ++i) {
                std::array<u8, 16> pair{};
                if (!fp || fp % 16 || fp > std::numeric_limits<u64>::max() - pair.size() ||
                    !read(fp, pair)) break;
                record.backtrace.push_back(Little64(pair.data() + 8));
                const auto next = Little64(pair.data());
                if (next <= fp) break;
                fp = next;
            }
        }
        if (records.size() == MaxRecords) {
            records.pop_front();
            ++evicted;
        }
        if (describe) {
            for (const auto address : {registers[32], registers[30]})
                record.locations.emplace_back(address, describe(address));
            for (const auto address : record.backtrace)
                record.locations.emplace_back(address, describe(address));
        }
        records.push_back(std::move(record));
        ++point.recorded;
    }
};

GuestTrace::GuestTrace() : impl{std::make_unique<Impl>()} {}
GuestTrace::~GuestTrace() = default;

std::string GuestTrace::Arm(u64 process_id, const Config& config, u64& id) {
    if ((config.target.empty() ? !InstructionAddress(config.address) : config.address != 0) ||
        config.target.size() > 512 || config.registers.size() > 34 ||
        config.captures.size() > 8 || !config.rate_limit || config.rate_limit > 10000 ||
        (config.counters_only && (config.on_return || !config.captures.empty() || config.backtrace))) {
        return "invalid_trace";
    }
    if (!config.target.empty()) {
        const auto separator = config.target.find_first_of("+!");
        if (separator == std::string::npos || separator == 0 || separator + 1 == config.target.size())
            return "invalid_target";
    }
    std::array<bool, 34> seen{};
    for (const auto index : config.registers) {
        if (index >= seen.size() || seen[index]) return "invalid_registers";
        seen[index] = true;
    }
    Impl::Entry entry;
    entry.point.config = config;
    std::string error;
    size_t total{};
    entry.point.config.captures.clear();
    for (auto capture : config.captures) {
        auto text = capture.expression;
        const auto type = capture.type == "bytes" ? std::string{"raw"} :
                          capture.type == "c-string" ? "c-string(" + std::to_string(capture.size) + ")" : capture.type;
        if (type == "raw" && text.find(':') == std::string::npos && capture.size)
            text += ":" + std::to_string(capture.size);
        auto compiled = TraceExpressions::Capture::Parse(text, type, error);
        const auto size = compiled ? compiled->Size() : 0;
        if (!compiled || !size || size > MaxCaptureBytes ||
            (total += size) > MaxCaptureBytes) return "invalid_capture";
        capture.size = static_cast<u32>(size);
        if (compiled->Type() == TraceExpressions::CaptureType::Raw) capture.type = "bytes";
        if (compiled->Type() == TraceExpressions::CaptureType::CString) capture.type = "c-string";
        entry.expressions.push_back(compiled->Address());
        entry.point.config.captures.push_back(std::move(capture));
    }
    if (!config.condition.empty()) {
        entry.condition = TraceExpressions::Condition::Parse(config.condition, error);
        if (!entry.condition) return "invalid_condition";
    }
    std::scoped_lock lock{impl->mutex};
    if (impl->points.size() == MaxPoints) return "trace_limit";
    entry.point.id = impl->next_id++;
    entry.point.process_id = process_id;
    entry.point.address = config.address;
    entry.point.state = config.address && process_id ? State::Armed : State::Pending;
    id = entry.point.id;
    impl->points.push_back(std::move(entry));
    impl->resolved = false;
    active.store(true, std::memory_order_release);
    return {};
}

bool GuestTrace::Disarm(u64 id) {
    std::scoped_lock lock{impl->mutex};
    bool found = !id;
    for (auto& entry : impl->points) {
        if (!id || entry.point.id == id) {
            entry.point.state = State::Disarmed;
            found = true;
        }
    }
    std::erase_if(impl->returns, [id](const auto& r) { return !id || r.point_id == id; });
    active.store(impl->HasWork(), std::memory_order_release);
    return found;
}

void GuestTrace::Clear() {
    std::scoped_lock lock{impl->mutex};
    impl->points.clear();
    impl->returns.clear();
    // Sequence numbers never reset, so existing readers cannot mistake a new
    // session's record for an already consumed one.
    impl->records.clear();
    impl->invalidations.clear();
    impl->invalidate_all = false;
    impl->resolved = false;
    active.store(false, std::memory_order_release);
}

void GuestTrace::BeginProcess(u64 process_id) {
    std::scoped_lock lock{impl->mutex};
    impl->process_id = process_id;
    impl->main_thread = 0;
    frame.store(0, std::memory_order_relaxed);
    for (auto& entry : impl->points) {
        if (entry.point.state == State::Pending && !entry.point.process_id) {
            entry.point.process_id = process_id;
            if (entry.point.address) entry.point.state = State::Armed;
        }
    }
    impl->resolved = false;
    active.store(impl->HasWork(), std::memory_order_release);
}

void GuestTrace::EndProcess() {
    Disarm(0);
}

void GuestTrace::SetMainThread(u64 thread_id) {
    std::scoped_lock lock{impl->mutex};
    impl->main_thread = thread_id;
}

std::vector<GuestTrace::Point> GuestTrace::List() const {
    std::scoped_lock lock{impl->mutex};
    std::vector<Point> result;
    for (const auto& entry : impl->points) result.push_back(entry.point);
    return result;
}

std::vector<u64> GuestTrace::ResolveTargets(u64 process_id, u64 revision, const Resolver& resolve) {
    if (!HasPoints()) return {};
    std::scoped_lock lock{impl->mutex};
    if (impl->resolved && impl->revision == revision && impl->process_id == process_id) return {};
    impl->process_id = process_id;
    impl->revision = revision;
    impl->resolved = true;
    std::vector<u64> changed;
    for (auto& entry : impl->points) {
        auto& point = entry.point;
        if ((point.state != State::Pending && point.state != State::Armed) ||
            point.config.target.empty() || (point.process_id && point.process_id != process_id)) continue;
        const auto address = resolve(point.config.target);
        const u64 next = address && InstructionAddress(*address) ? *address : 0;
        if (point.address != next) {
            std::erase_if(impl->returns, [&](const auto& r) { return r.point_id == point.id; });
            if (point.address) changed.push_back(point.address);
            if (next) changed.push_back(next);
        }
        point.process_id = process_id;
        point.address = next;
        point.state = next ? State::Armed : State::Pending;
    }
    for (const auto address : changed) {
        if (impl->invalidate_all) break;
        impl->invalidations.insert(address);
        if (impl->invalidations.size() > 1024) {
            impl->invalidations.clear();
            impl->invalidate_all = true;
        }
    }
    active.store(impl->HasWork(), std::memory_order_release);
    return changed;
}

GuestTrace::Invalidations GuestTrace::TakeInvalidations() {
    std::scoped_lock lock{impl->mutex};
    Invalidations result{impl->invalidate_all, {impl->invalidations.begin(), impl->invalidations.end()}};
    impl->invalidations.clear();
    impl->invalidate_all = false;
    return result;
}

bool GuestTrace::ShouldInstrument(u64 process_id, u64 address) const {
    if (!HasPoints()) return false;
    std::scoped_lock lock{impl->mutex};
    for (const auto& entry : impl->points) {
        const auto& point = entry.point;
        const bool awaiting_return = point.state == State::Completed &&
            std::ranges::any_of(impl->returns, [&](const auto& r) { return r.point_id == point.id; });
        if ((point.state == State::Armed || awaiting_return) && point.process_id == process_id && point.address == address)
            return true;
    }
    return std::ranges::any_of(impl->returns, [=](const auto& r) {
        return r.process_id == process_id && r.address == address;
    });
}

std::vector<u64> GuestTrace::Hit(u64 process_id, u64 thread_id, u32 core_id,
                               const Registers& registers, const MemoryReader& read,
                               Clock::time_point now, const Describer& describe) {
    if (!HasPoints()) return {};
    std::scoped_lock lock{impl->mutex};
    std::vector<u64> invalidate;
    // Return records are paired by thread, PC and entry SP; recursive calls may
    // share LR, but their frame SP differs. Unwound calls remain bounded.
    for (size_t i = impl->returns.size(); i > 0; --i) {
        const auto r = impl->returns[i - 1];
        if (r.process_id != process_id || r.thread_id != thread_id ||
            r.address != registers[32] || r.sp != registers[31]) continue;
        const auto entry = std::ranges::find_if(impl->points, [&](const auto& e) { return e.point.id == r.point_id; });
        if (entry != impl->points.end() && entry->point.state != State::Disarmed) {
            impl->RecordHit(*entry, thread_id, core_id, frame.load(std::memory_order_relaxed), registers, read, now, true, describe);
        }
        impl->returns.erase(impl->returns.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
    for (auto& entry : impl->points) {
        auto& point = entry.point;
        if ((point.state != State::Armed && point.state != State::Completed) ||
            point.process_id != process_id || point.address != registers[32]) continue;
        if (point.config.on_return) {
            const auto abandoned = std::erase_if(impl->returns, [&](const auto& r) {
                return r.point_id == point.id && r.process_id == process_id && r.thread_id == thread_id &&
                       r.address == registers[30] && r.sp == registers[31];
            });
            point.abandoned_returns += abandoned;
        }
        if (point.state != State::Armed) continue;
        ++point.hits;
        if (entry.condition && !entry.condition->Evaluate(registers, read, thread_id, impl->main_thread)) continue;
        ++point.matched;
        impl->RecordHit(entry, thread_id, core_id, frame.load(std::memory_order_relaxed), registers, read, now, false, describe);
        if (point.config.on_return) {
            if (impl->returns.size() == 1024 || !InstructionAddress(registers[30])) {
                ++point.return_overflow;
            } else {
                impl->returns.push_back({point.id, process_id, thread_id, registers[30], registers[31]});
                invalidate.push_back(registers[30]);
            }
        }
        if (point.config.max_hits && point.matched >= point.config.max_hits) point.state = State::Completed;
    }
    active.store(impl->HasWork(), std::memory_order_release);
    return invalidate;
}

GuestTrace::Batch GuestTrace::Read(u64 after, size_t limit) const {
    std::scoped_lock lock{impl->mutex};
    Batch batch;
    batch.next_sequence = after;
    batch.evicted = impl->evicted;
    if (impl->records.empty() && after < impl->sequence) {
        batch.lost = impl->sequence - after;
        batch.next_sequence = impl->sequence;
    }
    if (!impl->records.empty() && after < impl->records.front().sequence - 1)
        batch.lost = impl->records.front().sequence - 1 - after;
    for (const auto& record : impl->records) {
        if (record.sequence <= after) continue;
        if (batch.records.size() >= std::min(limit, size_t{128})) break;
        batch.records.push_back(record);
        batch.next_sequence = record.sequence;
    }
    return batch;
}

} // namespace Core
