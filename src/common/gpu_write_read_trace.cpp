// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/gpu_write_read_trace.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

namespace Common::GpuWriteReadTrace {
namespace {
std::unique_ptr<Recorder> recorder;
std::string Number(std::string_view key, Address value) {
    return ",\"" + std::string(key) + "\":" + std::to_string(value);
}
std::string Hex(std::string_view key, Address value) {
    std::ostringstream stream;
    stream << ",\"" << key << "\":\"0x" << std::hex << value << '"';
    return stream.str();
}
bool ValidRange(Address start, Address size) {
    return size != 0 && size <= std::numeric_limits<Address>::max() - start;
}
} // namespace

thread_local Context context;

const char* Name(Origin origin) {
    switch (origin) {
    case Origin::GuestA64: return "guest_a64_data";
    case Origin::CodeFetch: return "code_fetch";
    case Origin::HostBlock: return "host_read_block";
    case Origin::HostBlockUnsafe: return "host_read_block_unsafe";
    default: return "host_scalar";
    }
}

Recorder::Recorder(std::FILE* output_, std::size_t max_intervals_, Address max_bytes_)
    : output{output_}, max_intervals{max_intervals_}, max_bytes{max_bytes_} {
    Emit("\"type\":\"startup\",\"schema\":1,\"target_hash\":\"c478aab12efe386f\","
         "\"slot\":2,\"range_evidence\":\"declared_binding_not_actual_written_bytes\","
         "\"generation_kind\":\"writable_binding_observation_not_value_provenance\","
         "\"pc_quality\":\"approximate_unvalidated\",\"control_identity\":\"external_markers_required\","
         "\"ordering\":\"host_observation_not_guest_fence_proof\","
         "\"coverage_gaps\":\"raw_pointer_span_consumption_and_writes;CopyBlock_source;other_CPU_backends;"
         "other_GPU_writers;conservative_retirement_on_alias_unmap_and_cache_delete\""
         + Number("max_bytes", max_bytes) + Number("max_intervals", max_intervals));
    Summary("startup", true);
}

Recorder::~Recorder() {
    Summary("final", true);
    if (output) {
        std::fclose(output);
    }
}

void Recorder::Emit(std::string_view fields, bool final) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    const std::string line = "{\"seq\":" + std::to_string(++sequence) +
                             ",\"host_us\":" + std::to_string(elapsed) + "," +
                             std::string(fields) + "}\n";
    // Reserve room for the final totals even after event/periodic-summary saturation.
    const Address reserve = std::min<Address>(4096, max_bytes / 4);
    const Address limit = final ? max_bytes : max_bytes - reserve;
    if (io_error || counts.bytes + line.size() > limit) {
        ++counts.dropped_events;
        return;
    }
    if (output && std::fwrite(line.data(), 1, line.size(), output) != line.size()) {
        io_error = true;
        ++counts.dropped_events;
        std::fprintf(stderr, "GPU-write/read trace INVALID: output I/O failure\n");
        return;
    }
    counts.bytes += line.size();
    ++counts.events;
}

void Recorder::Split(Address at) {
    auto it = intervals.upper_bound(at);
    if (it == intervals.begin()) {
        return;
    }
    --it;
    if (it->first < at && at < it->second.end) {
        auto tail = it->second;
        it->second.end = at;
        intervals.emplace(at, tail);
    }
}

void Recorder::BoundState() {
    if (intervals.size() > max_intervals) {
        ++counts.overflow;
        Emit("\"type\":\"retire_all\",\"reason\":\"shadow_capacity_overflow_invalidates_zero\""
             + Number("intervals", intervals.size()));
        intervals.clear();
    }
}

void Recorder::RetireLocked(Address physical, Address size, std::string_view reason) {
    if (!ValidRange(physical, size)) {
        return;
    }
    const Address end = physical + size;
    Split(physical);
    Split(end);
    auto it = intervals.lower_bound(physical);
    while (it != intervals.end() && it->first < end) {
        Emit("\"type\":\"retire\",\"reason\":\"" + std::string(reason) + '"' +
             Hex("physical", it->first) + Number("size", it->second.end - it->first) +
             Number("generation", it->second.generation));
        ++counts.retirements;
        it = intervals.erase(it);
    }
    BoundState();
}

void Recorder::Retire(Address physical, Address size, std::string_view reason) {
    std::scoped_lock lock{mutex};
    RetireLocked(physical, size, reason);
}

Address Recorder::NewGeneration() {
    std::scoped_lock lock{mutex};
    return ++counts.generations;
}

void Recorder::Write(Address generation, const Mapping& mapping) {
    std::scoped_lock lock{mutex};
    if (!ValidRange(mapping.physical, mapping.size)) {
        ++counts.mapping_gaps;
        return;
    }
    RetireLocked(mapping.physical, mapping.size, "new_target_generation");
    intervals.emplace(mapping.physical,
                      Interval{mapping.physical + mapping.size, generation, 0, mapping});
    BoundState();
    Emit("\"type\":\"gpu_binding\",\"range_evidence\":\"declared_binding\"" +
         Number("generation", generation) + Hex("physical", mapping.physical) +
         Hex("device", mapping.device) + Hex("gpu", mapping.gpu) + Hex("cpu_backing", mapping.cpu) +
         Number("device_asid", mapping.address_space) + Number("size", mapping.size));
}

void Recorder::Capture(Address physical, Address size, Address device, Address commit,
                       std::vector<Snapshot>& result) {
    std::scoped_lock lock{mutex};
    if (!ValidRange(physical, size)) {
        return;
    }
    auto it = intervals.upper_bound(physical);
    if (it != intervals.begin()) {
        --it;
    }
    for (; it != intervals.end() && it->first < physical + size; ++it) {
        const Address begin = std::max(physical, it->first);
        const Address end = std::min(physical + size, it->second.end);
        if (begin >= end) {
            continue;
        }
        if (result.size() >= max_intervals) {
            ++counts.overflow;
            return;
        }
        result.push_back({begin, end - begin, device + begin - physical,
                          it->second.generation, commit});
        Emit("\"type\":\"download_commit\"" + Number("commit", commit) +
             Number("generation", it->second.generation) + Hex("physical", begin) +
             Number("size", end - begin));
    }
}

void Recorder::Deliver(const std::vector<Snapshot>& snapshot, Address device, Address size) {
    std::scoped_lock lock{mutex};
    if (!ValidRange(device, size)) {
        return;
    }
    for (const auto& part : snapshot) {
        const Address db = std::max(device, part.device);
        const Address de = std::min(device + size, part.device + part.size);
        if (db >= de) {
            continue;
        }
        const Address begin = part.physical + db - part.device;
        const Address end = begin + de - db;
        Split(begin);
        Split(end);
        for (auto it = intervals.lower_bound(begin); it != intervals.end() && it->first < end; ++it) {
            if (it->second.generation != part.generation) {
                continue; // An old pop must never deliver a newer write or a reused allocation.
            }
            it->second.delivered_commit = part.commit;
            ++counts.delivered_fragments;
            Emit("\"type\":\"download_pop\",\"ordering\":\"host_copy_only\"" +
                 Number("generation", part.generation) + Number("commit", part.commit) +
                 Hex("physical", it->first) + Number("size", it->second.end - it->first));
        }
        BoundState();
    }
}

void Recorder::Read(Address physical, Address size, Address cpu_va, const Context& access) {
    std::scoped_lock lock{mutex};
    if (!ValidRange(physical, size)) {
        return;
    }
    auto it = intervals.upper_bound(physical);
    if (it != intervals.begin()) {
        --it;
    }
    for (; it != intervals.end() && it->first < physical + size; ++it) {
        const Address begin = std::max(physical, it->first);
        const Address end = std::min(physical + size, it->second.end);
        if (begin >= end) {
            continue;
        }
        ++counts.matched_fragments;
        counts.matched_bytes += end - begin;
        if (access.origin == Origin::GuestA64) {
            ++counts.guest_fragments;
        } else if (access.origin == Origin::CodeFetch) {
            ++counts.code_fragments;
        } else {
            ++counts.host_fragments;
        }
        Emit("\"type\":\"read\",\"origin\":\"" + std::string(Name(access.origin)) +
             "\",\"control\":\"unclassified_use_external_markers\",\"pc_quality\":\"approximate\","
             "\"fence_relation\":\"unproven\"" + Hex("cpu_va", cpu_va) + Number("size", size) +
             Hex("access_va", access.access_va) + Number("access_size", access.access_size) +
             Hex("physical", physical) + Hex("overlap_physical", begin) +
             Number("overlap_size", end - begin) + Number("generation", it->second.generation) +
             Number("observed_pop_commit", it->second.delivered_commit) +
             Hex("pc", access.pc) + Hex("lr", access.lr) + Number("core", access.core) +
             Number("process", access.process));
    }
}

void Recorder::Coverage(std::string_view fields) {
    std::scoped_lock lock{mutex};
    Emit("\"type\":\"coverage\"," + std::string(fields));
    Flush();
}

void Recorder::Flush() {
    if (output && std::fflush(output) != 0 && !io_error) {
        io_error = true;
        ++counts.dropped_events;
        std::fprintf(stderr, "GPU-write/read trace INVALID: output flush failure\n");
    }
}

void Recorder::Gap(std::string_view reason) {
    std::scoped_lock lock{mutex};
    ++counts.mapping_gaps;
    Emit("\"type\":\"coverage_gap\",\"reason\":\"" + std::string(reason) + '"');
}

void Recorder::Summary(std::string_view phase, bool force) {
    std::scoped_lock lock{mutex};
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_summary < std::chrono::seconds{5}) {
        return;
    }
    last_summary = now;
    Emit("\"type\":\"summary\",\"phase\":\"" + std::string(phase) + '"' +
         Number("guest_callbacks", guest_callbacks.load(std::memory_order_relaxed)) +
         Number("generations", counts.generations) + Number("live_intervals", intervals.size()) +
         Number("matched_fragments", counts.matched_fragments) + Number("matched_bytes", counts.matched_bytes) +
         Number("guest_fragments", counts.guest_fragments) + Number("host_fragments", counts.host_fragments) +
         Number("code_fragments", counts.code_fragments) + Number("retirements", counts.retirements) +
         Number("delivered_fragments", counts.delivered_fragments) +
         Number("dropped_events", counts.dropped_events) + Number("overflow", counts.overflow) +
         Number("mapping_gaps", counts.mapping_gaps) + Number("bytes_before_summary", counts.bytes) +
         Number("events_before_summary", counts.events) +
         Number("saturated", counts.dropped_events != 0 || counts.overflow != 0 || io_error), phase == "final");
    Flush();
}

Counters Recorder::Totals() {
    std::scoped_lock lock{mutex};
    return counts;
}

bool InitializeFromEnvironment() {
    const char* gate = std::getenv("EDEN_TRACE_GPU_WRITE_READS");
    if (!gate || std::string_view{gate} != "1") {
        return false;
    }
    const char* path = std::getenv("EDEN_TRACE_GPU_WRITE_READS_PATH");
    const char* demand = std::getenv("EDEN_GNW_DEMAND_READBACK");
    const char* error = nullptr;
    if (demand && demand[0] == '1') {
        error = "conflict_EDEN_GNW_DEMAND_READBACK_must_be_OFF";
    } else if (!path || !std::filesystem::path{path}.is_absolute()) {
        error = "explicit_absolute_EDEN_TRACE_GPU_WRITE_READS_PATH_required";
    }
    if (error) {
        std::fprintf(stderr, "GPU-write/read trace INVALID: %s\n", error);
        return false;
    }
    // Exclusive create: do not truncate evidence from an earlier run.
    auto* file = std::fopen(path, "wx");
    if (!file) {
        std::fprintf(stderr, "GPU-write/read trace INVALID: cannot create output file\n");
        return false;
    }
    recorder = std::make_unique<Recorder>(file);
    return true;
}

Recorder& Get() {
    return *recorder;
}

} // namespace Common::GpuWriteReadTrace
