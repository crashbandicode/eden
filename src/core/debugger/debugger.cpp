// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/version.hpp>
#if BOOST_VERSION > 108400 && (!defined(_WINDOWS) && !defined(__ANDROID__)) || defined(YUZU_BOOST_v1)
#define USE_BOOST_v1
#endif
#ifdef USE_BOOST_v1
#include <boost/process/v1/async_pipe.hpp>
#else
#include <boost/process/async_pipe.hpp>
#endif

#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/debugger/debugger_interface.h"
#include "core/debugger/gdbstub.h"
#include "core/hle/kernel/global_scheduler_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"

/// Reads until an error; `c` gets the data, `on_error` the error that ended the loop (end of file
/// included). The callbacks may close `r`: the loop then ends with an error instead of reading again.
template <typename Readable, typename Buffer, typename Callback, typename ErrorCallback>
static void AsyncReceiveInto(Readable& r, Buffer& buffer, Callback&& c, ErrorCallback&& on_error) {
    static_assert(std::is_trivial_v<Buffer>);
    auto boost_buffer{boost::asio::buffer(&buffer, sizeof(Buffer))};
    r.async_read_some(boost_buffer, [&, c, on_error](const boost::system::error_code& error,
                                                     size_t bytes_read) {
        if (error.failed()) {
            on_error(error);
            return;
        }
        const u8* buffer_start = reinterpret_cast<const u8*>(&buffer);
        std::span<const u8> received_data{buffer_start, buffer_start + bytes_read};
        if (c(received_data)) {
            AsyncReceiveInto(r, buffer, c, on_error);
        }
    });
}

/// Accepts connections until the acceptor is closed. A failed accept (for example a peer that
/// reset before it was accepted) does not stop the server.
template <typename Callback>
static void AsyncAccept(boost::asio::ip::tcp::acceptor& acceptor, Callback&& c) {
    acceptor.async_accept([&, c](const boost::system::error_code& error, auto&& peer_socket) {
        if (error == boost::asio::error::operation_aborted || !acceptor.is_open()) {
            return;
        }
        if (error.failed()) {
            LOG_WARNING(Debug_GDBStub, "Accepting a debugger connection failed: {}", error.message());
        } else {
            c(peer_socket);
        }
        AsyncAccept(acceptor, c);
    });
}

/// A failed read or write on the debugger client's socket. Only that client is dropped.
struct ClientConnectionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class SignalType {
    Stopped,
    Watchpoint,
    ShuttingDown,
};

struct SignalInfo {
    SignalType type;
    Kernel::KThread* thread;
    const Kernel::DebugWatchpoint* watchpoint;
};

namespace Core {

class DebuggerImpl : public DebuggerBackend {
public:
    explicit DebuggerImpl(Core::System& system_, u16 port)
        : system{system_}
        , debug_process{system_.Kernel()}
    {
        InitializeServer(port);
    }

    ~DebuggerImpl() override {
        ShutdownServer();
    }

    bool SignalDebugger(SignalInfo signal_info) {
        std::scoped_lock lk{connection_lock};
        return SignalLocked(signal_info);
    }

    /// The application's main thread is about to run for the first time. It is held (suspended for
    /// debug) until a debugger continues or detaches, decided here under the connection lock so a
    /// client's continue can never land before the hold. A client that is already connected gets
    /// the launch as a stop of that thread.
    void ProcessLaunched(Kernel::KProcess* process, Kernel::KThread* main_thread) {
        std::scoped_lock lk{connection_lock};
        main_thread->RequestSuspend(system.Kernel(), Kernel::SuspendType::Debug);
        if (!state) {
            LOG_INFO(Debug_GDBStub, "Holding the guest at launch until a debugger attaches and continues");
            return;
        }
        if (debug_process.GetPointerUnsafe() != process) {
            debug_process = {system.Kernel(), process};
            static_cast<GDBStub*>(frontend.get())->SetDebugProcess(process);
        }
        state->active_thread = {system.Kernel(), main_thread};
        if (!SignalLocked(SignalInfo{SignalType::Stopped, main_thread, nullptr})) {
            // Already stopped for this client: its continue resumes the new thread with the rest.
            LOG_INFO(Debug_GDBStub, "Guest launched while the debugger is stopped; holding it");
        }
    }

    // These functions are callbacks from the frontend, and the lock will be held.
    // There is no need to relock it.

    std::span<const u8> ReadFromClient() override {
        boost::system::error_code error;
        auto boost_buffer{boost::asio::buffer(state->client_data)};
        const size_t bytes_read = state->client_socket.read_some(boost_buffer, error);
        if (error.failed()) {
            throw ClientConnectionError(fmt::format("read: {}", error.message()));
        }
        return {state->client_data.data(), bytes_read};
    }

    void WriteToClient(std::span<const u8> data) override {
        boost::system::error_code error;
        boost::asio::write(state->client_socket, boost::asio::buffer(data.data(), data.size_bytes()),
                           error);
        if (error.failed()) {
            throw ClientConnectionError(fmt::format("write: {}", error.message()));
        }
    }

    void SetActiveThread(Kernel::KThread* thread) override {
        state->active_thread = {system.Kernel(), thread};
    }

    Kernel::KThread* GetActiveThread() override {
        return state->active_thread.GetPointerUnsafe();
    }

private:
    void InitializeServer(u16 port) {
        using boost::asio::ip::tcp;

        LOG_INFO(Debug_GDBStub, "Starting server on port {}...", port);

        // Run the connection thread.
        connection_thread = std::jthread([&, port](std::stop_token stop_token) {
            Common::SetCurrentThreadName("Debugger");

            try {
                // Initialize the listening socket and accept a new client.
                tcp::endpoint endpoint{boost::asio::ip::address_v4::any(), port};
                tcp::acceptor acceptor{io_context, endpoint};

                AsyncAccept(acceptor, [&](auto&& peer) { AcceptConnection(std::move(peer)); });

                // A failure while serving one client drops that client; the server keeps listening.
                while (!stop_token.stop_requested()) {
                    try {
                        io_context.run();
                        break;
                    } catch (const std::exception& ex) {
                        LOG_ERROR(Debug_GDBStub, "Debugger connection error: {}", ex.what());
                        DropClient("connection error");
                        io_context.restart();
                    }
                }
            } catch (const std::exception& ex) {
                LOG_CRITICAL(Debug_GDBStub, "Stopping server: {}", ex.what());
            }
        });
    }

    void AcceptConnection(boost::asio::ip::tcp::socket&& peer) {
        LOG_INFO(Debug_GDBStub, "Accepting new peer connection");

        std::scoped_lock lk{connection_lock};

        if (state) {
            // One client at a time: the previous client is detached first.
            DropClientLocked("replaced by a new connection", false);
        }

        // Find the process we are going to debug.
        if (!SetDebugProcess()) {
            LOG_WARNING(Debug_GDBStub, "No guest process to debug yet; closing the connection");
            boost::system::error_code ignored;
            peer.close(ignored);
            return;
        }

        // Ensure everything is stopped.
        PauseEmulation();
        stopped = true;

        // Set up the new frontend.
        frontend = std::make_unique<GDBStub>(*this, system, debug_process.GetPointerUnsafe());

        // Set the new state.
        state = std::make_unique<ConnectionState>(std::move(peer), io_context, system.Kernel());
        const u64 generation = ++client_generation;

        // Set up the client signals for new data.
        AsyncReceiveInto(
            state->signal_pipe, state->pipe_data,
            [this, generation](auto d) { return PipeData(generation, d); }, [](auto) {});
        AsyncReceiveInto(
            state->client_socket, state->client_data,
            [this, generation](auto d) { return ClientData(generation, d); },
            [this, generation](const boost::system::error_code& error) {
                ClientEnded(generation, error);
            });

        // Set the active thread.
        UpdateActiveThread();

        // Set up the frontend.
        frontend->Connected();
    }

    /// The client's socket ended (closed, reset or failed): detach it, so it never leaves the
    /// guest stopped.
    void ClientEnded(u64 generation, const boost::system::error_code& error) {
        std::scoped_lock lk{connection_lock};
        if (!state || generation != client_generation) {
            return; // That client was already detached or replaced.
        }
        DropClientLocked(error == boost::asio::error::eof ? "client closed the connection"
                                                          : "connection " + error.message(),
                         true);
    }

    void DropClient(std::string_view reason) {
        std::scoped_lock lk{connection_lock};
        if (state) {
            DropClientLocked(reason, true);
        }
    }

    /// Detaches the connected client like a gdbserver detach: removes its breakpoints and
    /// watchpoints, resumes every thread of the debugged process (when `resume`), and closes the
    /// connection. The server keeps listening. The connection objects are destroyed only after the
    /// handler that is running now has returned.
    void DropClientLocked(std::string_view reason, bool resume) {
        LOG_WARNING(Debug_GDBStub, "Debugger client detached ({}){}", reason,
                    resume ? "; resuming the guest" : "");
        if (frontend) {
            static_cast<GDBStub*>(frontend.get())->RemoveBreakpoints();
        }
        if (resume && debug_process.GetPointerUnsafe() != nullptr) {
            ResumeEmulation();
        }
        stopped = false;
        ++client_generation;

        boost::system::error_code ignored;
        state->client_socket.shutdown(boost::asio::socket_base::shutdown_both, ignored);
        state->client_socket.close(ignored);
        state->signal_pipe.close(ignored);
        retired_states.push_back(std::move(state));
        retired_frontends.push_back(std::move(frontend));
        boost::asio::post(io_context, [this] {
            std::scoped_lock lk2{connection_lock};
            retired_states.clear();
            retired_frontends.clear();
        });
    }

    void ShutdownServer() {
        connection_thread.request_stop();
        io_context.stop();
        connection_thread.join();
    }

    /// Wakes the debugger thread with a stop or shutdown. Caller holds the connection lock.
    bool SignalLocked(SignalInfo signal_info) {
        if (stopped || !state) {
            // Do not notify the debugger about another event.
            // It should be ignored.
            return false;
        }

        // Set up the state.
        stopped = true;
        state->info = signal_info;

        // Write a single byte into the pipe to wake up the debug interface.
        boost::system::error_code error;
        boost::asio::write(state->signal_pipe, boost::asio::buffer(&stopped, sizeof(stopped)), error);
        if (error.failed()) {
            LOG_ERROR(Debug_GDBStub, "Could not signal the debugger thread: {}", error.message());
            stopped = false;
            return false;
        }

        return true;
    }

    /// Returns whether to keep reading the signal pipe.
    bool PipeData(u64 generation, std::span<const u8> data) {
        std::scoped_lock lk{connection_lock};
        if (!state || generation != client_generation) {
            return false;
        }

        try {
            switch (state->info.type) {
            case SignalType::Stopped:
            case SignalType::Watchpoint:
                // Stop emulation.
                PauseEmulation();

                // Notify the client.
                state->active_thread = {system.Kernel(), state->info.thread};
                UpdateActiveThread();

                if (state->info.type == SignalType::Watchpoint) {
                    frontend->Watchpoint(state->active_thread.GetPointerUnsafe(),
                                         *state->info.watchpoint);
                } else {
                    frontend->Stopped(state->active_thread.GetPointerUnsafe());
                }

                return true;
            case SignalType::ShuttingDown:
                frontend->ShuttingDown();

                // Release members.
                state->active_thread.Reset(nullptr);
                debug_process.Reset(nullptr);

                // Wait for emulation to shut down gracefully now.
                {
                    boost::system::error_code ignored;
                    state->signal_pipe.close(ignored);
                    state->client_socket.shutdown(boost::asio::socket_base::shutdown_both, ignored);
                }
                LOG_INFO(Debug_GDBStub, "Shut down server");
                return false;
            }
        } catch (const ClientConnectionError& ex) {
            DropClientLocked(ex.what(), state->info.type != SignalType::ShuttingDown);
        }
        return false;
    }

    /// Returns whether to keep reading the client socket.
    bool ClientData(u64 generation, std::span<const u8> data) {
        std::scoped_lock lk{connection_lock};
        if (!state || generation != client_generation) {
            return false;
        }

        try {
            return HandleClientActions(frontend->ClientData(data));
        } catch (const ClientConnectionError& ex) {
            DropClientLocked(ex.what(), true);
            return false;
        }
    }

    /// Returns whether the client stays connected.
    bool HandleClientActions(const std::vector<DebuggerAction>& actions) {
        for (const auto action : actions) {
            switch (action) {
            case DebuggerAction::Interrupt: {
                stopped = true;
                PauseEmulation();
                UpdateActiveThread();
                frontend->Stopped(state->active_thread.GetPointerUnsafe());
                break;
            }
            case DebuggerAction::Detach:
                DropClientLocked("detach requested", true);
                return false;
            case DebuggerAction::Continue:
                MarkResumed([&] { ResumeEmulation(); });
                break;
            case DebuggerAction::ContinueThreads: {
                auto* gdb = static_cast<GDBStub*>(frontend.get());
                MarkResumed([this, threads = std::move(gdb->resume_threads)] {
                    ResumeThreads(threads);
                });
                break;
            }
            case DebuggerAction::StepThread: {
                auto* gdb = static_cast<GDBStub*>(frontend.get());
                MarkResumed([this, threads = std::move(gdb->resume_threads)] {
                    auto* const thread = state->active_thread.GetPointerUnsafe();
                    if (thread) {
                        thread->SetStepState(Kernel::StepState::StepPending);
                        thread->Resume(system.Kernel(), Kernel::SuspendType::Debug);
                    }
                    ResumeThreads(threads, thread);
                });
                break;
            }
            case DebuggerAction::ShutdownEmulation: {
                // Spawn another thread that will exit after shutdown,
                // to avoid a deadlock
                Core::System* system_ref{&system};
                std::thread t([system_ref] { system_ref->Exit(); });
                t.detach();
                break;
            }
            }
        }
        return true;
    }

    void PauseEmulation() {
        if (debug_process.GetPointerUnsafe() == nullptr) {
            return;
        }
        Kernel::KScopedLightLock ll{debug_process->GetListLock()};
        Kernel::KScopedSchedulerLock sl{system.Kernel()};

        // Put all threads to sleep on next scheduler round.
        for (auto& thread : ThreadList()) {
            thread.RequestSuspend(system.Kernel(), Kernel::SuspendType::Debug);
        }
    }

    void ResumeEmulation(Kernel::KThread* except = nullptr) {
        if (debug_process.GetPointerUnsafe() == nullptr) {
            return;
        }
        Kernel::KScopedLightLock ll{debug_process->GetListLock()};
        Kernel::KScopedSchedulerLock sl{system.Kernel()};

        // Wake up all threads.
        for (auto& thread : ThreadList()) {
            if (std::addressof(thread) == except) {
                continue;
            }

            thread.SetStepState(Kernel::StepState::NotStepping);
            thread.Resume(system.Kernel(), Kernel::SuspendType::Debug);
        }
    }

    void ResumeThreads(const std::vector<Kernel::KThread*>& threads,
                       Kernel::KThread* except = nullptr) {
        if (debug_process.GetPointerUnsafe() == nullptr) {
            return;
        }
        Kernel::KScopedLightLock ll{debug_process->GetListLock()};
        Kernel::KScopedSchedulerLock sl{system.Kernel()};

        // Wake up only the specified threads.
        for (auto* thread : threads) {
            if (!thread || thread == except) {
                continue;
            }

            thread->SetStepState(Kernel::StepState::NotStepping);
            thread->Resume(system.Kernel(), Kernel::SuspendType::Debug);
        }
    }

    template <typename Callback>
    void MarkResumed(Callback&& cb) {
        stopped = false;
        cb();
    }

    void UpdateActiveThread() {
        if (debug_process.GetPointerUnsafe() == nullptr) {
            state->active_thread = {system.Kernel(), nullptr};
            return;
        }
        Kernel::KScopedLightLock ll{debug_process->GetListLock()};

        auto& threads{ThreadList()};
        for (auto& thread : threads) {
            if (std::addressof(thread) == state->active_thread.GetPointerUnsafe()) {
                // Thread is still alive, no need to update.
                return;
            }
        }
        // A process that has not started yet has no threads; its launch reports the main thread.
        state->active_thread = {system.Kernel(),
                                threads.empty() ? nullptr : std::addressof(threads.front())};
    }

private:
    /// Debugs the application process. Other processes (applets) are only a fallback: stopping
    /// or resuming them instead of the game would leave the game held.
    bool SetDebugProcess() {
        if (auto* const application = system.ApplicationProcess(); application != nullptr) {
            debug_process = {system.Kernel(), application};
            return true;
        }
        auto processes = system.Kernel().GetProcessList();
        if (processes.empty()) {
            return false;
        }
        debug_process = std::move(processes.back());
        return debug_process.GetPointerUnsafe() != nullptr;
    }

    Kernel::KProcess::ThreadList& ThreadList() {
        return debug_process->GetThreadList();
    }

private:
    System& system;
    Kernel::KScopedAutoObject<Kernel::KProcess> debug_process;
    std::unique_ptr<DebuggerFrontend> frontend;

    boost::asio::io_context io_context;
    std::jthread connection_thread;
    std::mutex connection_lock;

    struct ConnectionState {
#ifdef USE_BOOST_v1
        using async_pipe = boost::process::v1::async_pipe;
#else
        using async_pipe = boost::process::async_pipe;
#endif

        ConnectionState(boost::asio::ip::tcp::socket&& client_socket_, async_pipe signal_pipe_, Kernel::KernelCore& kernel)
            : client_socket{std::move(client_socket_)}
            , signal_pipe{std::move(signal_pipe_)}
            , active_thread{kernel, nullptr}
        {}

        boost::asio::ip::tcp::socket client_socket;
        async_pipe signal_pipe;
        SignalInfo info;
        Kernel::KScopedAutoObject<Kernel::KThread> active_thread;
        std::array<u8, 4096> client_data;
        bool pipe_data;
    };

    std::unique_ptr<ConnectionState> state{};
    /// Identifies the current client; handlers of a detached or replaced client see another value.
    u64 client_generation{};
    /// Detached clients' objects, destroyed once their handlers can no longer run.
    std::vector<std::unique_ptr<ConnectionState>> retired_states;
    std::vector<std::unique_ptr<DebuggerFrontend>> retired_frontends;
    bool stopped{};
};

Debugger::Debugger(Core::System& system, u16 port) {
    try {
        impl = std::make_unique<DebuggerImpl>(system, port);
    } catch (const std::exception& ex) {
        LOG_CRITICAL(Debug_GDBStub, "Failed to initialize debugger: {}", ex.what());
    }
}

Debugger::~Debugger() = default;

bool Debugger::NotifyThreadStopped(Kernel::KThread* thread) {
    return impl && impl->SignalDebugger(SignalInfo{SignalType::Stopped, thread, nullptr});
}

bool Debugger::NotifyThreadWatchpoint(Kernel::KThread* thread,
                                      const Kernel::DebugWatchpoint& watch) {
    return impl && impl->SignalDebugger(SignalInfo{SignalType::Watchpoint, thread, &watch});
}

void Debugger::NotifyShutdown() {
    if (impl) {
        impl->SignalDebugger(SignalInfo{SignalType::ShuttingDown, nullptr, nullptr});
    }
}

void Debugger::NotifyProcessLaunch(Kernel::KProcess* process, Kernel::KThread* main_thread) {
    if (!impl) {
        // No server is listening (the port could not be opened), so nothing could ever release a
        // hold: let the guest run.
        LOG_ERROR(Debug_GDBStub, "GDB stub is not listening; not holding the guest at launch");
        return;
    }
    impl->ProcessLaunched(process, main_thread);
}

} // namespace Core
