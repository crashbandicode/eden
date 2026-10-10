// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/gpu_write_read_trace.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace Trace = Common::GpuWriteReadTrace;

#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

namespace {

struct Fixture {
    std::FILE* file = std::tmpfile();
    Trace::Recorder recorder;
    explicit Fixture(std::size_t intervals = 4096, Trace::Address bytes = 64 * 1024 * 1024)
        : recorder{file, intervals, bytes} {
        CHECK(file);
    }
    std::string Log() {
        recorder.Summary("test", true);
        std::fflush(file);
        const auto end = std::ftell(file);
        CHECK(end >= 0);
        std::rewind(file);
        std::string result(static_cast<std::size_t>(end), '\0');
        CHECK(std::fread(result.data(), 1, result.size(), file) == result.size());
        CHECK(std::fseek(file, end, SEEK_SET) == 0);
        return result;
    }
    Trace::Address Write(Trace::Address physical = 0x80001004, Trace::Address size = 28) {
        const auto generation = recorder.NewGeneration();
        recorder.Write(generation, {.physical = physical, .size = size, .device = 0x2004,
                                    .gpu = 0x3004, .cpu = 0x4004, .address_space = 7});
        return generation;
    }
};

Trace::Context Guest(Trace::Address va = 0x4004, Trace::Address size = 4, Trace::Address pc = 0x100) {
    return {.origin = Trace::Origin::GuestA64, .access_va = va, .access_size = size, .pc = pc};
}

void DownloadDoesNotEraseReads() {
    Fixture f;
    const auto generation = f.Write();
    std::vector<Trace::Snapshot> snapshot;
    f.recorder.Capture(0x80001004, 28, 0x2004, 10, snapshot);
    CHECK(snapshot.size() == 1 && snapshot[0].generation == generation);
    f.recorder.Deliver(snapshot, 0x2004, 28);
    f.recorder.Read(0x80001004, 4, 0x4004, Guest());
    f.recorder.Read(0x80001004, 4, 0x9004, Guest(0x9004)); // second CPU VA, same backing
    CHECK(f.recorder.Totals().guest_fragments == 2);
    const auto log = f.Log();
    CHECK(log.find("\"observed_pop_commit\":10") != std::string::npos);
    CHECK(log.find("\"cpu_va\":\"0x9004\"") != std::string::npos);
    CHECK(log.find("\"fence_relation\":\"unproven\"") != std::string::npos);
}

void ExactBoundariesAndPartialOverwrite() {
    Fixture f;
    f.Write(); // [0x1004,0x1020)
    f.recorder.Read(0x80001000, 4, 0x4000, Guest());
    f.recorder.Read(0x80001020, 4, 0x4020, Guest());
    CHECK(f.recorder.Totals().matched_fragments == 0);
    f.recorder.Read(0x80001002, 4, 0x4002, Guest());
    f.recorder.Read(0x8000101e, 4, 0x401e, Guest());
    CHECK(f.recorder.Totals().matched_bytes == 4);
    std::vector<Trace::Snapshot> pending;
    f.recorder.Capture(0x80001004, 28, 0x2004, 3, pending);
    f.recorder.Retire(0x8000100c, 4, "cpu_overwrite");
    f.recorder.Deliver(pending, 0x2004, 28);
    CHECK(f.recorder.Totals().delivered_fragments == 2);
    f.recorder.Read(0x8000100c, 4, 0x400c, Guest());
    CHECK(f.recorder.Totals().matched_bytes == 4);
    f.recorder.Read(0x80001004, 28, 0x4004, Guest());
    CHECK(f.recorder.Totals().matched_bytes == 28); // remaining 8 + 16, never whole-page retirement
    f.recorder.Retire(0x80001000, 4096, "device_unmap");
    f.recorder.Read(0x80001004, 28, 0x4004, Guest());
    CHECK(f.recorder.Totals().matched_bytes == 28);
    CHECK(f.Log().find("\"reason\":\"device_unmap\"") != std::string::npos);
}

void OldPopCannotDeliverNewGeneration() {
    Fixture f;
    f.Write();
    std::vector<Trace::Snapshot> old;
    f.recorder.Capture(0x80001004, 28, 0x2004, 1, old);
    f.recorder.Retire(0x80001004, 28, "buffer_delete_or_gc");
    const auto newer = f.Write();
    CHECK(newer > old[0].generation);
    f.recorder.Deliver(old, 0x2004, 28);
    CHECK(f.recorder.Totals().delivered_fragments == 0);
    f.recorder.Read(0x80001004, 4, 0x4004, Guest());
    CHECK(f.Log().find("\"observed_pop_commit\":0") != std::string::npos);
    std::vector<Trace::Snapshot> current;
    f.recorder.Capture(0x80001004, 28, 0x2004, 2, current);
    f.recorder.Deliver(current, 0x2004, 4); // only the actually copied subrange
    f.recorder.Read(0x80001004, 4, 0x4004, Guest());
    f.recorder.Read(0x80001008, 4, 0x4008, Guest());
    CHECK(f.recorder.Totals().delivered_fragments == 1);
    CHECK(f.Log().find("\"observed_pop_commit\":2") != std::string::npos);
    // Rebind without unmap is also a fresh generation.
    f.Write();
    f.recorder.Deliver(current, 0x2004, 28);
    CHECK(f.recorder.Totals().delivered_fragments == 1);
}

void OriginsAndControlsStayDistinct() {
    Fixture f;
    f.Write();
    // Control and natural guest loads are both actual guest loads. Preserve raw identity;
    // only the parent control markers can classify them, never an invented PC heuristic.
    f.recorder.Read(0x80001004, 4, 0x4004, Guest(0x4004, 4, 0x111));
    f.recorder.Read(0x80001004, 4, 0x4004, Guest(0x4004, 4, 0x222));
    f.recorder.Read(0x80001004, 4, 0x4004, {.origin = Trace::Origin::HostBlock});
    f.recorder.Read(0x80001004, 4, 0x4004, {.origin = Trace::Origin::HostBlockUnsafe});
    f.recorder.Read(0x80001004, 4, 0x4004, {.origin = Trace::Origin::CodeFetch});
    CHECK(f.recorder.Totals().guest_fragments == 2);
    CHECK(f.recorder.Totals().host_fragments == 2);
    CHECK(f.recorder.Totals().code_fragments == 1);
    const auto log = f.Log();
    CHECK(log.find("\"pc\":\"0x111\"") != std::string::npos);
    CHECK(log.find("\"pc\":\"0x222\"") != std::string::npos);
    CHECK(log.find("unclassified_use_external_markers") != std::string::npos);
    CHECK(log.find("host_read_block_unsafe") != std::string::npos);
}

void BoundsAndConcurrentCounters() {
    Fixture f{2, 8192};
    f.Write();
    f.Write(0x80002004);
    f.Write(0x80003004);
    CHECK(f.recorder.Totals().overflow == 1);
    f.recorder.Read(0x80001004, 4, 0x4004, Guest());
    CHECK(f.recorder.Totals().matched_fragments == 0);
    f.Write();
    auto read = [&] {
        for (int i = 0; i < 100; ++i) {
            f.recorder.guest_callbacks.fetch_add(1, std::memory_order_relaxed);
            f.recorder.Read(0x80001004, 4, 0x4004, Guest());
        }
    };
    std::thread a{read}, b{read};
    a.join();
    b.join();
    CHECK(f.recorder.guest_callbacks == 200);
    CHECK(f.recorder.Totals().matched_fragments == 200);
    CHECK(f.recorder.Totals().dropped_events > 0);
    f.recorder.Summary("final", true);
    CHECK(f.recorder.Totals().bytes <= 8192);
    const auto log = f.Log();
    CHECK(log.find("\"phase\":\"final\"") != std::string::npos);
    CHECK(log.find("\"guest_callbacks\":200") != std::string::npos);
    CHECK(log.find("\"saturated\":1") != std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const bool expected = std::string_view{argv[1]} == "--expect-enabled";
            CHECK(Trace::Enabled() == expected);
            {
                Trace::AccessScope guest{Guest()};
                if (expected) {
                    CHECK(Trace::context.origin == Trace::Origin::GuestA64);
                    Trace::AccessScope code{{.origin = Trace::Origin::CodeFetch}};
                    CHECK(Trace::context.origin == Trace::Origin::CodeFetch);
                }
            }
            CHECK(Trace::context.origin == Trace::Origin::HostScalar);
            if (expected) {
                CHECK(Trace::Get().guest_callbacks == 1);
            }
        } else {
            DownloadDoesNotEraseReads();
            ExactBoundariesAndPartialOverwrite();
            OldPopCannotDeliverNewGeneration();
            OriginsAndControlsStayDistinct();
            BoundsAndConcurrentCounters();
        }
        std::puts("gpu_write_read_trace: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "gpu_write_read_trace: FAIL: %s\n", error.what());
        return 1;
    }
}
