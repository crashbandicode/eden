// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace Common::GpuWriteReadTrace {

using Address = std::uint64_t;
constexpr Address TargetHash = 0xc478aab12efe386fULL;
constexpr std::uint32_t TargetCbufOffset = 0x310 + 16 * 2;

// Addresses in the shadow are physical DRAM addresses, not CPU/GPU VAs or host pointers.
enum class Origin { HostScalar, GuestA64, CodeFetch, HostBlock, HostBlockUnsafe };
const char* Name(Origin origin);

struct Context {
    Origin origin{Origin::HostScalar};
    Address access_va{}, access_size{}, pc{}, lr{}, process{};
    std::uint32_t core{~std::uint32_t{0}}; // unknown for host-only accesses
};
extern thread_local Context context;

struct Mapping {
    Address physical{}, size{}, device{}, gpu{}, cpu{}, address_space{};
};

struct Snapshot {
    Address physical{}, size{}, device{}, generation{}, commit{};
};

struct Counters {
    Address generations{}, matched_fragments{}, matched_bytes{}, guest_fragments{}, host_fragments{},
        code_fragments{}, retirements{}, delivered_fragments{}, dropped_events{}, overflow{},
        mapping_gaps{}, events{}, bytes{};
};

// Also used directly by the focused host test: it needs no GPU, core or logging dependencies.
class Recorder {
public:
    explicit Recorder(std::FILE* output = nullptr, std::size_t max_intervals = 4096,
                      Address max_bytes = 64 * 1024 * 1024);
    ~Recorder();
    Address NewGeneration();
    void Write(Address generation, const Mapping& mapping);
    void Retire(Address physical, Address size, std::string_view reason);
    void Capture(Address physical, Address size, Address device, Address commit,
                 std::vector<Snapshot>& result);
    void Deliver(const std::vector<Snapshot>& snapshot, Address device, Address size);
    void Read(Address physical, Address size, Address cpu_va, const Context& access);
    void Coverage(std::string_view fields);
    void Gap(std::string_view reason);
    void Summary(std::string_view phase, bool force = false);
    Counters Totals();
    std::atomic<Address> guest_callbacks{};

private:
    struct Interval {
        Address end{}, generation{}, delivered_commit{};
        Mapping mapping;
    };
    void Split(Address at);
    void RetireLocked(Address physical, Address size, std::string_view reason);
    void Emit(std::string_view fields, bool final = false);
    void Flush();
    void BoundState();
    std::mutex mutex;
    std::map<Address, Interval> intervals;
    std::FILE* output;
    const std::size_t max_intervals;
    const Address max_bytes;
    Counters counts;
    Address sequence{};
    bool io_error{};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()}, last_summary{start};
};

// The off gate does not construct a recorder or open a file. Environment is immutable per run.
bool InitializeFromEnvironment();
inline bool Enabled() {
    static const bool enabled = InitializeFromEnvironment();
    return enabled;
}
Recorder& Get();

class AccessScope {
public:
    explicit AccessScope(Context access) : enabled{Enabled()} {
        if (enabled) {
            previous = context;
            context = access;
            if (access.origin == Origin::GuestA64) {
                Get().guest_callbacks.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    ~AccessScope() {
        if (enabled) {
            context = previous;
        }
    }
    AccessScope(const AccessScope&) = delete;
    AccessScope& operator=(const AccessScope&) = delete;
private:
    bool enabled;
    Context previous;
};

} // namespace Common::GpuWriteReadTrace
