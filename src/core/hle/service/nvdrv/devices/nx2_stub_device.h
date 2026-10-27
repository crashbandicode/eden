// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "core/hle/service/nvdrv/devices/nvdevice.h"

namespace Service::Nvidia::Devices {

// Set once a Switch 2 NvRm has identified itself (0xD0-byte GetCharacteristics). Devices then
// answer ioctls they don't know with a logged, input-echoing success instead of NotImplemented.
inline std::atomic<bool> g_nx2_driver{false};

inline NvResult Nx2UnknownIoctl(const char* device, Ioctl command, std::span<const u8> input,
                                std::span<u8> output) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) Switch 2 {} ioctl={:08X} group={:01X} command={:02X} in={:#x}",
                device, command.raw, command.group, command.cmd, input.size());
    std::fill(output.begin(), output.end(), u8{0});
    std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
    return NvResult::Success;
}

// GPU mappings a Switch 2 process made through nx2:gpu: CreateGpuAccessibleMemory (sub 101) maps
// the memory at GPU address == CPU address, and NormalMapGpuMemory4K (211) maps a memory handle at
// a chosen GPU address (also in sparse regions). Every nvhost-as-gpu address space applies each
// mapping, including ones made before the address space existed.
class Nx2GpuMemoryRegistry {
public:
    using Listener = std::function<void(u64 gpu_address, u64 cpu_address, u64 size)>;

    static Nx2GpuMemoryRegistry& Get() {
        static Nx2GpuMemoryRegistry registry;
        return registry;
    }

    void Register(u64 address, u64 size) {
        MapAt(address, address, size);
    }

    void MapAt(u64 gpu_address, u64 cpu_address, u64 size) {
        std::scoped_lock lk{mutex};
        mappings.push_back({gpu_address, cpu_address, size});
        for (auto& [owner, listener] : listeners) {
            listener(gpu_address, cpu_address, size);
        }
    }

    void AddListener(const void* owner, Listener listener) {
        std::scoped_lock lk{mutex};
        for (const auto& m : mappings) {
            listener(m.gpu_address, m.cpu_address, m.size);
        }
        listeners.insert_or_assign(owner, std::move(listener));
    }

    void RemoveListener(const void* owner) {
        std::scoped_lock lk{mutex};
        listeners.erase(owner);
    }

private:
    struct Mapping {
        u64 gpu_address;
        u64 cpu_address;
        u64 size;
    };

    std::mutex mutex;
    std::vector<Mapping> mappings;
    std::map<const void*, Listener> listeners;
};

// Device node that Switch 2 nvgpu drivers open but Switch 1 never had (e.g. /dev/nvhost-tsg-gpu).
// Every ioctl succeeds with a zeroed reply and is logged, so the driver's use of it shows up.
class nx2_stub_device final : public nvdevice {
public:
    explicit nx2_stub_device(Core::System& system_, std::string name_)
        : nvdevice{system_}, name{std::move(name_)} {}

    NvResult Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                    std::span<const u8> inline_input, std::span<u8> output) override {
        return Stub(fd, command, input, output, {});
    }
    NvResult Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                    std::span<u8> inline_output) override {
        return Stub(fd, command, input, output, inline_output);
    }

    void OnOpen(NvCore::SessionId session_id, DeviceFD fd) override {
        LOG_WARNING(Service_NVDRV, "(STUBBED) {} opened as fd {}", name, fd);
    }
    void OnClose(DeviceFD fd) override {}

private:
    NvResult Stub(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output,
                  std::span<u8> inline_output) {
        LOG_WARNING(Service_NVDRV, "(STUBBED) {} fd={} ioctl={:08X} group={:01X} command={:02X} in={:#x}",
                    name, fd, command.raw, command.group, command.cmd, input.size());
        // Echo the input (in/out ioctls keep their fields), zero the rest.
        std::fill(output.begin(), output.end(), u8{0});
        std::copy_n(input.begin(), std::min(input.size(), output.size()), output.begin());
        std::fill(inline_output.begin(), inline_output.end(), u8{0});
        return NvResult::Success;
    }

    std::string name;
};

} // namespace Service::Nvidia::Devices
