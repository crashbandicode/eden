// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/nv01_timer.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"

namespace Tegra {
struct DmaPusherTestAccess {
    static bool Step(DmaPusher& pusher) {
        return pusher.Step();
    }
};
} // namespace Tegra

namespace {
using Tegra::CommandHeader;
using Tegra::SubmissionMode;

CommandHeader Header(u32 method, u32 count, SubmissionMode mode, u32 subchannel = 5) {
    CommandHeader result{};
    result.method.Assign(method);
    result.subchannel.Assign(subchannel);
    result.arg_count.Assign(count);
    result.mode.Assign(mode);
    return result;
}

CommandHeader Argument(u32 value) {
    CommandHeader result{};
    result.argument = value;
    return result;
}

struct Fixture {
    Core::System system;
    Core::DeviceMemory device_memory;
    Tegra::MaxwellDeviceMemoryManager device_manager{device_memory};
    Tegra::MemoryManager memory{system, device_manager, 32, 1ULL << 32, 16, 16};
    // Channel payload contains large engine register arrays; keep it off the stack.
    std::unique_ptr<Tegra::Control::ChannelState> channel =
        std::make_unique<Tegra::Control::ChannelState>(0);

    Fixture() {
        channel->memory_manager = &memory;
        channel->Init(system, 0);
        Submit({Header(0, 1, SubmissionMode::Increasing),
                Argument(static_cast<u32>(Tegra::EngineID::NV01_TIMER))});
    }

    void Submit(std::initializer_list<CommandHeader> words) {
        boost::container::small_vector<CommandHeader, 512> commands(words);
        auto& pusher = channel->payload->dma_pusher;
        pusher.Push(Tegra::CommandList{std::move(commands)});
        REQUIRE(Tegra::DmaPusherTestAccess::Step(pusher));
        REQUIRE(pusher.dma_pushbuffer.empty());
    }

    auto& Timer() {
        return channel->payload->nv01_timer;
    }
};
} // namespace

TEST_CASE("NV01 timer commands do not accumulate across submissions", "[gpu][timer-sink]") {
    auto fixture = std::make_unique<Fixture>();
    REQUIRE(fixture->channel->payload->dma_pusher.subchannels[5] == &fixture->Timer());
    for (u32 batch = 0; batch < 4096; ++batch) {
        fixture->Submit({Header(0x40, 3, SubmissionMode::Increasing),
                         Argument(batch), Argument(batch + 1), Argument(batch + 2)});
        fixture->Submit({Header(0x1fff, batch & 0x1fff, SubmissionMode::Inline)});
        // A command split across two pushbuffers must preserve parser continuation.
        fixture->Submit({Header(0x80, 2, SubmissionMode::Increasing), Argument(batch)});
        fixture->Submit({Argument(batch + 1)});
        REQUIRE(fixture->Timer().method_sink.empty());
        REQUIRE(fixture->Timer().method_sink.capacity() == 0);
    }
}

TEST_CASE("NV01 timer drains deferred writes before a multi-method submission", "[gpu][timer-sink]") {
    auto fixture = std::make_unique<Fixture>();
    // Exercise the inherited drain contract as well as the normal no-queue path.
    fixture->Timer().method_sink.emplace_back(0x40, 7);
    fixture->Timer().method_sink.emplace_back(0x41, 8);
    fixture->Submit({Header(0x60, 3, SubmissionMode::NonIncreasing),
                     Argument(11), Argument(12), Argument(13)});
    CHECK(fixture->Timer().method_sink.empty());
    fixture->Submit({Header(0x70, 3, SubmissionMode::IncreaseOnce),
                     Argument(21), Argument(22), Argument(23)});
    CHECK(fixture->Timer().method_sink.empty());
}

TEST_CASE("Timer dispatch preserves another engine's deferred command ordering", "[gpu][timer-sink]") {
    class RecordingEngine final : public Tegra::Engines::EngineInterface {
    public:
        RecordingEngine() {
            execution_mask.set(0x80);
        }
        void CallMethod(Core::System&, u32 method, u32 value, bool) override {
            received.emplace_back(method, value);
        }
        void CallMultiMethod(Core::System& system, u32 method, const u32* values, u32 count,
                             u32) override {
            for (u32 i = 0; i < count; ++i) {
                CallMethod(system, method, values[i], true);
            }
        }
        std::vector<std::pair<u32, u32>> received;
    } peer;

    auto fixture = std::make_unique<Fixture>();
    fixture->channel->payload->dma_pusher.BindSubchannel(
        &peer, 6, Tegra::Engines::EngineTypes::Fermi2D);
    fixture->Submit({Header(0x40, 2, SubmissionMode::Increasing, 6), Argument(1), Argument(2)});
    CHECK(peer.received.empty());
    REQUIRE(peer.method_sink.size() == 2);
    fixture->Submit({Header(0x40, 3, SubmissionMode::Inline)});
    CHECK(peer.received.empty());
    CHECK(fixture->Timer().method_sink.empty());
    fixture->Submit({Header(0x80, 1, SubmissionMode::Increasing, 6), Argument(3)});
    const std::vector<std::pair<u32, u32>> expected{{0x40, 1}, {0x41, 2}, {0x80, 3}};
    CHECK(peer.received == expected);
    CHECK(peer.method_sink.empty());
}
