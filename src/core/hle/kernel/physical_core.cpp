// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <limits>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/debugger/guest_tracepoint.h"
#include "core/debugger/guest_trace.h"
#include "core/debugger/trace_fault.h"
#include "core/debugger/trace_modules.h"
#include "core/arm/debug.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"

namespace Kernel {

PhysicalCore::PhysicalCore(KernelCore& kernel, std::size_t core_index)
    : m_core_index{core_index}
{
    m_is_single_core = !kernel.IsMulticore();
}
PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::RunThread(KernelCore& kernel, Kernel::KThread* thread) {
    auto* process = thread->GetOwnerProcess();
    auto& system = kernel.System();
    auto* interface = process->GetArmInterface(m_core_index);

    interface->Initialize();
    // Retention is valid only inside this dispatch, never across scheduler,
    // SVC, debugger, migration or other real CPU exits.
    auto continuation_guard = SCOPE_GUARD { interface->FinishTracepointContinuation(); };
    const auto finish_before_scheduling = [&] {
        interface->FinishTracepointContinuation();
        continuation_guard.Cancel();
    };

    const auto EnterContext = [&]() {
        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // Check if we are already interrupted. If we are, we can just stop immediately.
        if (m_is_interrupted) {
            return false;
        }

        // Mark that we are running.
        m_arm_interface = interface;
        m_current_thread = thread;

        // Acquire the lock on the thread parameters.
        // This allows us to force synchronization with Interrupt.
        interface->LockThread(thread);

        return true;
    };

    const auto ExitContext = [&]() {
        // Save the JIT context back to the thread so the debugger can
        // read the current register state. Debug halt paths (step,
        // breakpoint, watchpoint) return from RunThread without going
        // through the scheduler's Unload/SaveContext.
        if (system.DebuggerEnabled()) {
            interface->GetContext(thread->GetContext());
        }

        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_arm_interface = nullptr;
        m_current_thread = nullptr;
    };

    while (true) {
        // If the thread is scheduled for termination, exit.
        if (thread->HasDpc() && thread->IsTerminationRequested()) {
            finish_before_scheduling();
            thread->Exit(kernel);
        }

        // Notify the debugger and go to sleep if a step was performed
        // and this thread has been scheduled again.
        if (thread->GetStepState() == StepState::StepPerformed) {
            finish_before_scheduling();
            system.GetDebugger().NotifyThreadStopped(thread);
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Otherwise, run the thread.
        Core::HaltReason hr{};
        const bool trace_step = thread->IsTracepointStepPending();
        {
            // If we were interrupted, exit immediately.
            if (!EnterContext()) {
                return;
            }

            if (!system.GetUnmappedAccess().EnterExecution(process->GetProcessId(), m_core_index)) {
                hr = Core::HaltReason::UnmappedStop;
            } else if (trace_step) {
                hr = interface->StepTracepointThread(thread);
                // An external interrupt may arrive before the instruction ran.
                // Retain the resume obligation across scheduling in that case.
                if (True(hr & (Core::HaltReason::StepThread | Core::HaltReason::SupervisorCall |
                               Core::HaltReason::InstructionBreakpoint | Core::HaltReason::PrefetchAbort |
                               Core::HaltReason::DataAbort))) {
                    thread->SetTracepointStepPending(false);
                }
                if (True(hr & Core::HaltReason::StepThread) &&
                    thread->GetStepState() == StepState::StepPending) {
                    thread->SetStepState(StepState::StepPerformed);
                }
            } else if (thread->GetStepState() == StepState::StepPending) {
                hr = interface->StepThread(thread);

                if (True(hr & Core::HaltReason::StepThread)) {
                    thread->SetStepState(StepState::StepPerformed);
                }
            } else {
                hr = interface->RunThread(thread);
            }

            ExitContext();
        }

        if (True(hr & Core::HaltReason::UnmappedStop)) {
            system.GetUnmappedAccess().LeaveExecution(m_core_index);
            // Separate from debugger watchpoints: there is no watchpoint object.
            // The JIT has already aborted before the invalid access commits.
            interface->GetContext(thread->GetContext());
            finish_before_scheduling();
            process->RequestDiagnosticStop(kernel);
            return;
        }

        if (True(hr & Core::HaltReason::Tracepoint)) {
            // A tracepoint is a host translation boundary, not a guest BRK.
            // RaiseException sets PC to the trace address + 4 without executing
            // the instruction. Read after Run returns so host FP exception flags
            // have been synchronized, then rewind only that PC.
            auto& context = thread->GetContext();
            interface->GetContext(context);
            context.pc -= sizeof(u32);
            interface->SetContext(context);
            Core::GuestTracepoint::Registers registers{};
            std::copy(context.r.begin(), context.r.end(), registers.begin());
            registers[29] = context.fp;
            registers[30] = context.lr;
            registers[31] = context.sp;
            registers[32] = context.pc;
            registers[33] = context.pstate;
            const auto read = [process](u64 address, std::span<u8> bytes) {
                    // Deliberately CPU-visible bytes. GPU flushing would turn a
                    // bounded diagnostic into a potentially blocking readback.
                    auto& memory = process->GetMemory();
                    if (address > std::numeric_limits<u64>::max() - bytes.size()) return false;
                    for (size_t offset = 0; offset < bytes.size(); offset += 256) {
                        if (!memory.ReadBlockForDiagnostic(address + offset,
                                bytes.subspan(offset, std::min(size_t{256}, bytes.size() - offset)))) return false;
                    }
                    return true;
                };
            system.GetGuestTracepoint().Capture(process->GetProcessId(), thread->GetThreadId(), registers, read);
            const auto new_addresses = system.GetGuestTrace().Hit(
                process->GetProcessId(), thread->GetThreadId(), static_cast<u32>(m_core_index), registers, read,
                Core::GuestTrace::Clock::now(), [&](u64 address) {
                    const auto location = system.GetTraceModules().Describe(process->GetProcessId(), address);
                    return location ? location->text : std::string{};
                });
            for (const auto address : new_addresses) Core::InvalidateInstructionCacheRange(process, address, sizeof(u32));
            if (!system.GetGuestTrace().ShouldInstrument(process->GetProcessId(), context.pc)) {
                interface->InvalidateCacheRange(context.pc, sizeof(u32));
            }
            // Execute the intercepted instruction through the separate single-step
            // cache, then return to persistent instrumented code. Never patch code
            // or temporarily remove a point shared with another core.
            thread->SetTracepointStepPending(true);
        }

        // Finish observations belonging to the exited instruction before
        // publishing quiescence; no new guest entry can pass a latched fault.
        system.GetUnmappedAccess().LeaveExecution(m_core_index);

        // Determine why we stopped.
        // If a step completed successfully, skip other halt reason handlers
        // the step takes priority (e.g. step may also set InstructionBreakpoint
        // if the next instruction happens to be a breakpoint).
        const bool step_completed = True(hr & Core::HaltReason::StepThread)
                                    && thread->GetStepState() == StepState::StepPerformed && !trace_step;
        const bool supervisor_call = !step_completed && True(hr & Core::HaltReason::SupervisorCall);
        const bool prefetch_abort = !step_completed && True(hr & Core::HaltReason::PrefetchAbort);
        const bool breakpoint = !step_completed && True(hr & Core::HaltReason::InstructionBreakpoint);
        const bool data_abort = !step_completed && True(hr & Core::HaltReason::DataAbort);
        const bool interrupt = !step_completed && True(hr & Core::HaltReason::BreakLoop);

        // Since scheduling may occur here, we cannot use any cached
        // state after returning from calls we make.
        if (breakpoint || prefetch_abort || data_abort || supervisor_call) {
            finish_before_scheduling();
        }

        // Notify the debugger and go to sleep if a breakpoint was hit,
        // or if the thread is unable to continue for any reason.
        if (breakpoint || prefetch_abort) {
            if (breakpoint) {
                interface->RewindBreakpointInstruction();
                // RewindBreakpointInstruction sets the JIT state to the
                // saved breakpoint context. Update the thread context to
                // match, since ExitContext already saved the post-execution
                // state.
                interface->GetContext(thread->GetContext());
            }
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadStopped(thread);
            } else {
                interface->LogBacktrace(process);
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort) {
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadWatchpoint(thread, *interface->HaltedWatchpoint());
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Handle system calls.
        if (supervisor_call) {
            // Perform call.
            Svc::Call(system, interface->GetSvcNumber());
            return;
        }

        // Handle external interrupt sources.
        const bool observation_boundary = hr == Core::HaltReason::Tracepoint ||
                                          (trace_step && hr == Core::HaltReason::StepThread);
        if (interrupt || (m_is_single_core && !observation_boundary)) {
            return;
        }
    }
}

void PhysicalCore::LoadContext(const KThread* thread) {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->SetContext(thread->GetContext());
        interface->SetTpidrroEl0(GetInteger(thread->GetTlsAddress()));
        interface->SetWatchpointArray(&process->GetWatchpoints());
    }
}

void PhysicalCore::LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args) {
    process.GetArmInterface(m_core_index)->SetSvcArguments(args);
}

void PhysicalCore::ClearSvcScratchRegisters(const KProcess& process) {
    process.GetArmInterface(m_core_index)->ClearSvcScratchRegisters();
}

void PhysicalCore::SaveContext(KThread* thread) const {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->GetContext(thread->GetContext());
    }
}

void PhysicalCore::SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const {
    process.GetArmInterface(m_core_index)->GetSvcArguments(args);
}

void PhysicalCore::CloneFpuStatus(KThread* dst) const {
    auto* process = dst->GetOwnerProcess();

    Svc::ThreadContext ctx{};
    process->GetArmInterface(m_core_index)->GetContext(ctx);

    dst->GetContext().fpcr = ctx.fpcr;
    dst->GetContext().fpsr = ctx.fpsr;
}

void PhysicalCore::LogBacktrace(KernelCore& kernel) {
    auto* process = GetCurrentProcessPointer(kernel);
    if (!process) {
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->LogBacktrace(process);
    }
}

void PhysicalCore::Idle() {
    std::unique_lock lk{m_guard};
    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
}

bool PhysicalCore::IsInterrupted() const {
    return m_is_interrupted;
}

void PhysicalCore::Interrupt() {
    // Lock core context.
    std::scoped_lock lk{m_guard};

    // Load members.
    auto* arm_interface = m_arm_interface;
    auto* thread = m_current_thread;

    // Add interrupt flag.
    m_is_interrupted = true;

    // Interrupt ourselves.
    m_on_interrupt.notify_one();

    // If there is no thread running, we are done.
    if (arm_interface == nullptr) {
        return;
    }

    // Interrupt the CPU.
    arm_interface->SignalInterrupt(thread);
}

void PhysicalCore::ClearInterrupt() {
    std::scoped_lock lk{m_guard};
    m_is_interrupted = false;
}

} // namespace Kernel
