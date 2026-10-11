//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The bottom layer of the synchronization primitives: SuspensionKind and SuspendableBase, which
// keep the suspended tasks of one object (a lock-free stack for Multiple, one atomic slot for
// Single) on top of Core.h SuspendTask / SuspendTaskFor / ResumeTask.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <atomic>
#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.SuspendableBase;

import ThinkMeta.ConcurrentTasks.Collections.BasicStack;
import ThinkMeta.ConcurrentTasks.Collections.ConcurrentStack;
import ThinkMeta.ConcurrentTasks.Cpp.Padded;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    // A stack of suspended task handles (ConcurrentStack or BasicStack of SuspendedTaskEntry).
    template <typename T>
    concept SuspendedTaskStack = requires(T a) {
        { a.Push(static_cast<SuspendedTask>(nullptr)) } -> std::same_as<void>;
        { a.Pop() } -> std::same_as<SuspendedTask>;
    };

    /// <summary>How many tasks may be suspended on one synchronization object at the same time.</summary>
    /// <remarks>
    /// Every primitive comes in both variants: the plain name (<see cref="SuspensionKind::Multiple"/>)
    /// and the "_s" name (<see cref="SuspensionKind::Single"/>).
    /// </remarks>
    /// <seealso cref="SuspendableBase"/>
    export enum class SuspensionKind {
        /// <summary>At most one suspended task at a time.</summary>
        /// <remarks>
        /// Asserted in Debug by the events. Cheaper, and the only variant that offers waits with a
        /// timeout.
        /// </remarks>
        Single,

        /// <summary>Any number of suspended tasks, up to 65535 at a time.</summary>
        /// <remarks>Woken in LIFO order, no fairness. No timeouts.</remarks>
        Multiple
    };

    /// <summary>Keeps the tasks suspended on one synchronization object and resumes them.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of suspended tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one suspended task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// Bottom building block of the synchronization primitives, on top of Core.h
    /// <see cref="SuspendTask"/>.
    /// <see cref="Suspend"/> / <see cref="SuspendFor"/> must be called from a task or from an OS
    /// thread converted with <see cref="ConvertThreadToTask"/>; the resume members may be called
    /// from any thread. Destroy the object only when no task is suspended on it any more (asserted
    /// in Debug).
    /// </remarks>
    /// <seealso cref="SuspensionKind"/>
    /// <seealso cref="Suspendable"/>
    export template <SuspensionKind _SuspensionKind>
    class CT_NOVTABLE SuspendableBase;

    // Multiple: the suspended tasks are chained through their SuspendedTaskEntry (see Core.h) on a
    // ConcurrentStack, so pushing and popping need no allocation.
    template <>
    class CT_NOVTABLE SuspendableBase<SuspensionKind::Multiple> : protected ISuspendCallback
    {
        SuspendableBase(const SuspendableBase&)            = delete;
        SuspendableBase(SuspendableBase&&)                 = delete;
        SuspendableBase& operator=(const SuspendableBase&) = delete;
        SuspendableBase& operator=(SuspendableBase&&)      = delete;

    protected:
        /// <summary>Creates the object with no suspended task.</summary>
        /// <remarks>Works anywhere, also for global and static objects.</remarks>
        SuspendableBase() noexcept = default;

        /// <summary>Destroys the object.</summary>
        /// <remarks>No task may be suspended on the object any more (asserted in Debug).</remarks>
        ~SuspendableBase() noexcept { CT_ASSERT(!IsSuspended()); }

        /// <summary>Suspends the calling task.</summary>
        /// <remarks>
        /// The core then calls <see cref="ISuspendCallback::OnSuspended"/>. Task or converted thread
        /// only.
        /// </remarks>
        void Suspend() noexcept { SuspendTask(this); }

        /// <summary>Resumes every suspended task.</summary>
        /// <remarks>Covers the tasks suspended right now.</remarks>
        /// <seealso cref="ResumeNext"/>
        /// <seealso cref="ResumeNonReentrant"/>
        void Resume() noexcept { ResumeTasks(suspendedTasks_); }

        /// <summary>Resumes one suspended task.</summary>
        /// <returns><c>true</c> if a task was resumed; <c>false</c> if there was none.</returns>
        /// <remarks>The most recently suspended task is resumed.</remarks>
        /// <seealso cref="Resume"/>
        bool ResumeNext() noexcept
        {
            auto taskContext = suspendedTasks_.Pop();
            if (taskContext)
                ResumeTask(taskContext);

            return taskContext != nullptr;
        }

        /// <summary>Hands a lock over to one suspended task and resumes it.</summary>
        /// <param name="tc">
        /// Receives the popped task (or <c>nullptr</c>) before the task is resumed;
        /// <see cref="CriticalSection"/> passes its owner field.
        /// </param>
        /// <returns><c>true</c> if a task was resumed; <c>false</c> if there was no suspended task.</returns>
        bool ResumeNext(std::atomic<TaskHandle>& tc) noexcept
        {
            auto taskContext = suspendedTasks_.Pop();

            // Publish the ownership transfer BEFORE resuming the next task.
            // Otherwise the resumed task could run concurrently (on another
            // scheduler thread) while this task is still finishing the hand-off,
            // leading to a data race on the non-atomic lockCount_ and a stale
            // lockingTaskContext_ that is never cleared (lost wake-up / hang).
            tc.exchange(taskContext, std::memory_order_release);

            if (taskContext)
                ResumeTask(taskContext);

            return taskContext != nullptr;
        }

        /// <summary>Resumes the tasks suspended right now, but not those that suspend meanwhile.</summary>
        /// <remarks>
        /// Like <see cref="Resume"/>, but first moves the suspended tasks to a local stack, so that
        /// tasks which suspend again while the loop runs are not resumed by this call. Reverses
        /// the order: the earliest suspended task is resumed first.
        /// </remarks>
        void ResumeNonReentrant() noexcept
        {
            Collections::BasicStack<SuspendedTaskEntry> stack;

            while (auto taskContext = suspendedTasks_.Pop())
                stack.Push(taskContext);

            ResumeTasks(stack);
        }

        /// <summary>Tells whether any task is suspended.</summary>
        /// <returns><c>true</c> if at least one task is suspended (a snapshot; may change at once).</returns>
        [[nodiscard]] bool IsSuspended() const noexcept { return GetNumSuspendedTasks() != 0; }

        /// <summary>Returns the number of suspended tasks.</summary>
        /// <returns>The number of tasks suspended on the object, at most 65535.</returns>
        /// <remarks>A snapshot (<c>QueryDepthSList</c>).</remarks>
        [[nodiscard]] unsigned short GetNumSuspendedTasks() const noexcept { return suspendedTasks_.Size(); }

        /// <summary>Publishes a suspended task.</summary>
        /// <param name="suspendedTask">
        /// The handle <see cref="ISuspendCallback::OnSuspended"/> received; not <c>nullptr</c>.
        /// </param>
        /// <remarks>Called from <see cref="ISuspendCallback::OnSuspended"/>.</remarks>
        void AddSuspendedTask(SuspendedTask suspendedTask) noexcept { suspendedTasks_.Push(suspendedTask); }

    private:
        template <SuspendedTaskStack Stack>
        void ResumeTasks(Stack& stack) noexcept
        {
            // The object must outlive this loop: a resumed task may not destroy it while another
            // thread still resumes (the users destroy it only once no Resume can run any more).
            while (auto taskContext = stack.Pop())
                ResumeTask(taskContext);
        }

        Collections::ConcurrentStack<SuspendedTaskEntry> suspendedTasks_;
    };

    // Single: one atomic slot holds the suspended task. Every resumer claims it with an exchange (or
    // the timeout with a compare-exchange), so exactly one of them resumes the task.
    template <>
    class CT_NOVTABLE SuspendableBase<SuspensionKind::Single> : protected ISuspendCallback
    {
    protected:
        /// <summary>Creates the object with an empty slot.</summary>
        SuspendableBase() noexcept = default;

        /// <summary>Destroys the object.</summary>
        /// <remarks>No task may be suspended on the object any more (asserted in Debug).</remarks>
        ~SuspendableBase() noexcept { CT_ASSERT(!IsSuspended()); }

        SuspendableBase(const SuspendableBase&)            = delete;
        SuspendableBase(SuspendableBase&&)                 = delete;
        SuspendableBase& operator=(const SuspendableBase&) = delete;
        SuspendableBase& operator=(SuspendableBase&&)      = delete;

        /// <summary>Suspends the calling task.</summary>
        /// <remarks>
        /// The core then calls <see cref="ISuspendCallback::OnSuspended"/>. Task or converted thread
        /// only.
        /// </remarks>
        /// <seealso cref="SuspendFor"/>
        void Suspend() noexcept { SuspendTask(this); }

        /// <summary>Suspends the calling task with a time limit.</summary>
        /// <param name="timeout">
        /// Longest time to stay suspended, as a <see cref="Duration"/>; <see cref="Duration::Infinite"/>
        /// waits without a timeout.
        /// </param>
        /// <returns>
        /// <c>true</c> if a resumer ended the wait; <c>false</c> if it timed out (see
        /// <see cref="SuspendTaskFor"/>).
        /// </returns>
        /// <seealso cref="Suspend"/>
        bool SuspendFor(Duration timeout) noexcept { return SuspendTaskFor(this, timeout); }

        // ISuspendCallback: the timeout claims the task the same way a resumer does, so exactly one
        // of them gets it.
        bool OnTimeout(SuspendedTask suspendedTask) noexcept override
        {
            return suspendedTask_.compare_exchange_strong(suspendedTask, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        /// <summary>Claims and resumes the suspended task.</summary>
        /// <returns><c>true</c> if a task was resumed; <c>false</c> if the slot was empty.</returns>
        bool Resume() noexcept
        {
            // Atomically claim the suspended task so that two concurrent resumers
            // (e.g. a producer calling Set() and the suspending task's own scheduler
            // thread re-checking via TryResume()) can never both hand off the same
            // TaskContext*, which would double-enqueue it and corrupt the scheduler.
            auto suspendedTask = suspendedTask_.exchange(nullptr, std::memory_order_acq_rel);

            if (suspendedTask) {
                Resume(suspendedTask);
                return true;
            }

            return false;
        }

        /// <summary>Resumes a task that the caller has already claimed.</summary>
        /// <param name="suspendedTask">The claimed handle; <c>nullptr</c> is ignored.</param>
        /// <remarks>Calls <see cref="ResumeTask"/>.</remarks>
        static void Resume(SuspendedTask suspendedTask) noexcept { ResumeTask(suspendedTask); }

        /// <summary>Same as <see cref="Resume"/>().</summary>
        /// <returns><c>true</c> if a task was resumed; <c>false</c> if the slot was empty.</returns>
        /// <remarks>The name the <see cref="SuspensionKind::Multiple"/> variant uses for "resume one".</remarks>
        bool ResumeNext() noexcept { return Resume(); }

        /// <summary>Hands a lock over to the suspended task and resumes it.</summary>
        /// <param name="tc">
        /// Receives the claimed task (or <c>nullptr</c>) before the task is resumed;
        /// <see cref="CriticalSection_s"/> passes its owner field.
        /// </param>
        /// <returns><c>true</c> if a task was resumed; <c>false</c> if the slot was empty.</returns>
        bool ResumeNext(std::atomic<TaskHandle>& tc) noexcept
        {
            // Atomically claim the suspended task (see Resume() above).
            auto taskContext = suspendedTask_.exchange(nullptr, std::memory_order_acq_rel);

            // Publish the ownership transfer BEFORE resuming the next task.
            // Otherwise the resumed task could run concurrently (on another
            // scheduler thread) while this task is still finishing the hand-off,
            // leading to a data race on the non-atomic lockCount_ and a stale
            // lockingTaskContext_ that is never cleared (lost wake-up / hang).
            tc.exchange(taskContext, std::memory_order_release);

            if (taskContext)
                ResumeTask(taskContext);

            return taskContext != nullptr;
        }

        /// <summary>Tells whether a task is suspended.</summary>
        /// <returns><c>true</c> if a task is in the slot (a snapshot; may change at once).</returns>
        [[nodiscard]] bool IsSuspended() const noexcept { return suspendedTask_.load(std::memory_order_acquire) != nullptr; }

        /// <summary>Records the suspended task.</summary>
        /// <param name="suspendedTask">
        /// The handle <see cref="ISuspendCallback::OnSuspended"/> received; not <c>nullptr</c>.
        /// </param>
        /// <remarks>
        /// Called from <see cref="ISuspendCallback::OnSuspended"/>. The slot must be empty: at most
        /// one task may be suspended on the object at a time (asserted in Debug).
        /// </remarks>
        void SetSuspendedTask(SuspendedTask suspendedTask) noexcept
        {
            // Dekker-style pairing: this exchange comes before the caller's TryResume reads the
            // object's state; the signalling side writes its state (an RMW too) and then reads or
            // claims the slot, so at least one side sees the other (no lost wake-up). This relies on
            // both RMWs being full barriers, as lock-prefixed instructions are on x86; the C++ orders
            // used here (release/acquire) do not by themselves exclude that both sides miss.
            [[maybe_unused]] auto previous = suspendedTask_.exchange(suspendedTask, std::memory_order_release);
            CT_ASSERT(!previous);
        }

    private:
        Cpp::Padded<std::atomic<SuspendedTask>> suspendedTask_{nullptr};
    };
} // namespace ThinkMeta::ConcurrentTasks