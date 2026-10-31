// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression probe for the GPU sync-fence wait at shutdown (video_core/gpu.cpp).
//
// Production contract under test:
//   * GPU::RunOnGpuThread queues an action and returns its fence. GPU::TickWork, which the GPU
//     thread runs between command lists, executes queued actions and advances
//     GPU::CurrentSyncRequestFence. GPU::WaitForSyncOperation blocks until that fence is reached.
//   * Core::System::Impl::ShutdownMainProcess calls GPU::NotifyShutdown after the GPU thread has
//     been joined and before guest CPU threads are suspended. A guest thread parked in, or
//     entering, WaitForSyncOperation in that window has no producer left. NotifyShutdown must
//     release it, or KernelCore::SuspendEmulation never completes and the frontend never sees the
//     emulation thread finish.
//
// The scenarios drive the real GPU front end without a renderer and without a GPU thread: the GPU
// constructor needs neither (Start() would). is_async is true so that TickGPU's command push does
// not block on the never-started thread's stop token. Because the baseline behaviour under test is
// an indefinite wait, every scenario runs in a child process: this test binary is re-executed with
// one hidden child test selected. The child bounds its own waits and hard-exits with kHangExitCode
// when a waiter is still blocked, so a failing baseline yields a prompt, reported failure and never
// leaves a parked thread behind in the test runner. The parent kills a child that exceeds its
// overall budget. The child-process harness is implemented for Linux; elsewhere the parent tests
// are skipped and the child tests stay hidden.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "core/core.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/gpu_thread.h"

#if defined(__linux__)
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

// Exit status the child uses when a sync waiter is still blocked at its deadline. Distinct from
// Catch2's own exit codes, which count failed assertions.
constexpr int kHangExitCode = 99;
constexpr auto kWaiterDeadline = 5s;

bool WaitUntil(const std::atomic<bool>& flag, std::chrono::milliseconds deadline) {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// Ends the child without running destructors. A thread parked inside GPU::WaitForSyncOperation
// would otherwise block the join in ~std::thread, or be torn down under the GPU it waits on.
[[noreturn]] void ExitForHang(const char* waiter) {
    std::fprintf(stderr,
                 "gpu_shutdown_sync: %s still blocked in GPU::WaitForSyncOperation after %lld ms; "
                 "NotifyShutdown did not release the sync waiter\n",
                 waiter,
                 static_cast<long long>(std::chrono::milliseconds(kWaiterDeadline).count()));
    std::fflush(stderr);
    std::_Exit(kHangExitCode);
}

// The GPU front end as the emulator holds it before Start(): no renderer, no GPU thread.
struct Fixture {
    Core::System system;
    Tegra::GPU gpu{system, /*is_async=*/true, /*use_nvdec=*/false};
};

constexpr auto SplitStopped = VideoCommon::GpuSplit::ExecutionState::Stopped;

} // namespace

// ---- Child scenarios: hidden, run by the parent tests below in a separate process ------------

TEST_CASE("GpuSyncWait_child_ordinary_fence", "[.][gpu_shutdown_sync_child]") {
    // Declared before the fixture so the queued action's capture outlives the GPU that holds it.
    std::atomic<int> runs{0};
    Fixture f;

    const u64 fence =
        f.gpu.RunOnGpuThread([&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(f.gpu.CurrentSyncRequestFence() < fence);

    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        f.gpu.WaitForSyncOperation(fence);
        released.store(true, std::memory_order_release);
    });
    if (!WaitUntil(entered, kWaiterDeadline)) {
        ExitForHang("ordinary waiter did not start");
    }

    // Without a tick the waiter must stay parked: no shutdown escape may fire while the GPU is
    // not shutting down. Timing based: a spurious release is caught, a slow waiter is not.
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(released.load(std::memory_order_acquire));
    CHECK(runs.load(std::memory_order_relaxed) == 0);

    // What the GPU thread does between command lists.
    f.gpu.TickWork();
    if (!WaitUntil(released, kWaiterDeadline)) {
        ExitForHang("completed-fence waiter");
    }
    waiter.join();
    CHECK(runs.load(std::memory_order_relaxed) == 1);
    CHECK(f.gpu.CurrentSyncRequestFence() == fence);

    // Shutdown after completion is benign, and a satisfied fence never waits.
    f.gpu.NotifyShutdown();
    f.gpu.WaitForSyncOperation(fence);
    CHECK(VideoCommon::GpuSplit::Get().GetStatus() == SplitStopped);
}

TEST_CASE("GpuSyncWait_child_parked_waiters_at_shutdown", "[.][gpu_shutdown_sync_child]") {
    std::atomic<int> runs{0};
    Fixture f;

    // Two outstanding fences: NotifyShutdown must wake every parked waiter, not just one.
    const u64 first =
        f.gpu.RunOnGpuThread([&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
    const u64 second =
        f.gpu.RunOnGpuThread([&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
    REQUIRE(first < second);

    std::atomic<bool> first_released{false};
    std::atomic<bool> second_released{false};
    std::atomic<bool> first_entered{false};
    std::atomic<bool> second_entered{false};
    std::thread first_waiter([&] {
        first_entered.store(true, std::memory_order_release);
        f.gpu.WaitForSyncOperation(first);
        first_released.store(true, std::memory_order_release);
    });
    std::thread second_waiter([&] {
        second_entered.store(true, std::memory_order_release);
        f.gpu.WaitForSyncOperation(second);
        second_released.store(true, std::memory_order_release);
    });
    if (!WaitUntil(first_entered, kWaiterDeadline) ||
        !WaitUntil(second_entered, kWaiterDeadline)) {
        ExitForHang("parked waiters did not start");
    }

    // Let both park. Nothing ticks: at this point of ShutdownMainProcess the GPU thread has
    // already been joined, so these fences can never be reached.
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(first_released.load(std::memory_order_acquire));
    CHECK_FALSE(second_released.load(std::memory_order_acquire));

    f.gpu.NotifyShutdown();

    if (!WaitUntil(first_released, kWaiterDeadline)) {
        ExitForHang("first parked waiter");
    }
    if (!WaitUntil(second_released, kWaiterDeadline)) {
        ExitForHang("second parked waiter");
    }
    first_waiter.join();
    second_waiter.join();

    // Released by shutdown, not by completion: no action ran and the fences were never reached.
    // The queued actions are destroyed unexecuted with the GPU. Production callers such as
    // OnCPURead and GetAppletCaptureBuffer capture stack locals the same way, which is why a
    // released waiter's action must never execute afterwards.
    CHECK(runs.load(std::memory_order_relaxed) == 0);
    CHECK(f.gpu.CurrentSyncRequestFence() < first);
    CHECK(VideoCommon::GpuSplit::Get().GetStatus() == SplitStopped);
}

TEST_CASE("GpuSyncWait_child_waiter_entering_after_shutdown", "[.][gpu_shutdown_sync_child]") {
    std::atomic<int> runs{0};
    Fixture f;

    f.gpu.NotifyShutdown();
    CHECK(VideoCommon::GpuSplit::Get().GetStatus() == SplitStopped);

    // A guest thread reaching the GPU after the join still queues its request (the queue is
    // process memory, not the thread) and must not wait for a producer that no longer exists.
    const u64 fence =
        f.gpu.RunOnGpuThread([&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
    std::atomic<bool> released{false};
    std::thread waiter([&] {
        f.gpu.WaitForSyncOperation(fence);
        released.store(true, std::memory_order_release);
    });
    if (!WaitUntil(released, kWaiterDeadline)) {
        ExitForHang("late waiter");
    }
    waiter.join();
    CHECK(runs.load(std::memory_order_relaxed) == 0);
    CHECK(f.gpu.CurrentSyncRequestFence() < fence);

    // Same entry shape as OnCPURead and the Qt shader-cache reload: request, tick and wait in one
    // production call.
    std::atomic<bool> idle_released{false};
    std::thread idle_waiter([&] {
        f.gpu.WaitForIdle();
        idle_released.store(true, std::memory_order_release);
    });
    if (!WaitUntil(idle_released, kWaiterDeadline)) {
        ExitForHang("WaitForIdle after shutdown");
    }
    idle_waiter.join();
}

TEST_CASE("GpuSyncWait_child_full_command_queue", "[.][gpu_shutdown_sync_child]") {
    Core::System system;
    // The 4096-entry queue contains inline command-list storage; like GPU::Impl,
    // keep it on the heap rather than exceeding the host thread's stack limit.
    auto manager = std::make_unique<VideoCommon::GPUThread::ThreadManager>(system);
    // No consumer runs. Fill the production queue, then force a producer to wait
    // inside PushCommand rather than inside the later GPU sync-fence wait.
    for (std::size_t i = 0; i < Common::detail::DefaultCapacity; ++i) {
        manager->TickGPU(true);
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::thread producer([&] {
        entered.store(true, std::memory_order_release);
        manager->TickGPU(true);
        returned.store(true, std::memory_order_release);
    });
    if (!WaitUntil(entered, kWaiterDeadline)) {
        ExitForHang("queue producer did not start");
    }
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(returned.load(std::memory_order_acquire));
    manager->NotifyShutdown();
    if (!WaitUntil(returned, kWaiterDeadline)) {
        std::fprintf(stderr, "gpu_shutdown_sync: producer blocked in full command queue after shutdown\n");
        std::fflush(stderr);
        std::_Exit(kHangExitCode);
    }
    producer.join();

    // A drain alone is insufficient: new producers must not fill a dead queue.
    std::atomic<bool> late_returned{false};
    std::thread late([&] {
        for (std::size_t i = 0; i <= Common::detail::DefaultCapacity; ++i) {
            manager->TickGPU(true);
        }
        late_returned.store(true, std::memory_order_release);
    });
    if (!WaitUntil(late_returned, kWaiterDeadline)) {
        std::fprintf(stderr, "gpu_shutdown_sync: late producer refilled a stopped command queue\n");
        std::fflush(stderr);
        std::_Exit(kHangExitCode);
    }
    late.join();
}

TEST_CASE("GpuSyncWait_child_synchronous_command", "[.][gpu_shutdown_sync_child]") {
    Core::System system;
    auto manager = std::make_unique<VideoCommon::GPUThread::ThreadManager>(system);
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::thread producer([&] {
        entered.store(true, std::memory_order_release);
        manager->TickGPU(false);
        returned.store(true, std::memory_order_release);
    });
    if (!WaitUntil(entered, kWaiterDeadline)) {
        ExitForHang("synchronous producer did not start");
    }
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(returned.load(std::memory_order_acquire));
    manager->NotifyShutdown();
    if (!WaitUntil(returned, kWaiterDeadline)) {
        std::fprintf(stderr, "gpu_shutdown_sync: synchronous command waiter blocked after shutdown\n");
        std::fflush(stderr);
        std::_Exit(kHangExitCode);
    }
    producer.join();
}

// ---- Parent harness ----------------------------------------------------------------------------

#if defined(__linux__)
namespace {

constexpr auto kChildBudget = 30s;

struct ChildRun {
    bool timed_out{};
    bool signaled{};
    int status{}; // exit code, or the signal number when signaled
    std::string output;
};

ChildRun RunChildTest(const char* test_name) {
    ChildRun run{};

    std::string exe(4096, '\0');
    const ssize_t exe_len = readlink("/proc/self/exe", exe.data(), exe.size() - 1);
    REQUIRE(exe_len > 0);
    exe.resize(static_cast<std::size_t>(exe_len));

    // Everything the child needs is prepared before fork(); the child only redirects and execs.
    std::string name(test_name);
    char* const argv[] = {exe.data(), name.data(), nullptr};

    std::string output_path =
        (std::filesystem::temp_directory_path() / "eden-gpu-shutdown-sync-XXXXXX").string();
    std::vector<char> output_template(output_path.begin(), output_path.end());
    output_template.push_back('\0');
    const int output_fd = mkstemp(output_template.data());
    REQUIRE(output_fd >= 0);
    output_path.assign(output_template.data());

    std::fflush(nullptr);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(output_fd, STDOUT_FILENO);
        dup2(output_fd, STDERR_FILENO);
        close(output_fd);
        execv(exe.c_str(), argv);
        _exit(127);
    }
    close(output_fd);

    const auto deadline = std::chrono::steady_clock::now() + kChildBudget;
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            break;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Never report a child we lost track of as a pass.
            run.status = -1;
            run.output = "waitpid failed: errno " + std::to_string(errno);
            unlink(output_path.c_str());
            return run;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            run.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    if (WIFSIGNALED(status)) {
        run.signaled = true;
        run.status = WTERMSIG(status);
    } else if (WIFEXITED(status)) {
        run.status = WEXITSTATUS(status);
    }

    {
        std::ifstream in(output_path);
        std::stringstream buffer;
        buffer << in.rdbuf();
        run.output = buffer.str();
    }
    unlink(output_path.c_str());
    return run;
}

void ExpectChildPass(const char* test_name) {
    const ChildRun run = RunChildTest(test_name);
    INFO("child test: " << test_name);
    INFO("child output:\n" << run.output);
    REQUIRE_FALSE(run.timed_out);
    REQUIRE_FALSE(run.signaled);
    if (run.status == kHangExitCode) {
        FAIL("GPU sync waiter stayed blocked through NotifyShutdown (baseline hang reproduced)");
    }
    if (run.status == 127) {
        FAIL("child could not re-execute the test binary");
    }
    REQUIRE(run.status == 0);
}

} // namespace
#endif

TEST_CASE("GPU sync wait: a completed fence releases its waiter and later shutdown is benign",
          "[video_core][gpu_shutdown_sync]") {
#if defined(__linux__)
    ExpectChildPass("GpuSyncWait_child_ordinary_fence");
#else
    SKIP("child-process harness is implemented for Linux only");
#endif
}

TEST_CASE("GPU sync wait: NotifyShutdown releases waiters parked on unreachable fences",
          "[video_core][gpu_shutdown_sync]") {
#if defined(__linux__)
    ExpectChildPass("GpuSyncWait_child_parked_waiters_at_shutdown");
#else
    SKIP("child-process harness is implemented for Linux only");
#endif
}

TEST_CASE("GPU sync wait: a waiter entering after NotifyShutdown does not block",
          "[video_core][gpu_shutdown_sync]") {
#if defined(__linux__)
    ExpectChildPass("GpuSyncWait_child_waiter_entering_after_shutdown");
#else
    SKIP("child-process harness is implemented for Linux only");
#endif
}

TEST_CASE("GPU sync wait: shutdown releases full-queue producers and rejects late commands",
          "[video_core][gpu_shutdown_sync]") {
#if defined(__linux__)
    ExpectChildPass("GpuSyncWait_child_full_command_queue");
#else
    SKIP("child-process harness is implemented for Linux only");
#endif
}

TEST_CASE("GPU sync wait: shutdown releases a synchronous command waiter",
          "[video_core][gpu_shutdown_sync]") {
#if defined(__linux__)
    ExpectChildPass("GpuSyncWait_child_synchronous_command");
#else
    SKIP("child-process harness is implemented for Linux only");
#endif
}
