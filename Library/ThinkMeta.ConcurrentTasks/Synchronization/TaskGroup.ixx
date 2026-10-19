//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// TaskGroup (any number of waiters) and TaskGroup_s (one waiter, timeouts): counts tasks that have
// not exited yet and lets Wait pass once all of them have (fork/join).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <atomic>
#include <concepts>
#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

import ThinkMeta.ConcurrentTasks.Synchronization.Waitable;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>The task counter of <see cref="TaskGroup"/> / <see cref="TaskGroup_s"/>.</summary>
    /// <remarks>The group is notified when its tasks exit.</remarks>
    class CT_NOVTABLE TaskGroupBase : public INotifyTaskStateChange
    {
    public:
        /// <summary>Adds a task to the group.</summary>
        /// <param name="task">
        /// The task to count in; the group becomes its exit callback
        /// (<see cref="ITask::SetNotifyCallback"/>, which replaces any callback set before: a task
        /// belongs to one group at a time).
        /// </param>
        /// <remarks>
        /// Call it before the task is added to a scheduler. A task of the group may add further
        /// tasks while it runs, since the group cannot complete before it exits. From outside the
        /// group, add tasks only while the group cannot complete, or before anyone waits for the
        /// round they belong to. Never suspends; any thread.
        /// Only an exit counts a task out: if it is then not started (a start by time that returns
        /// <c>false</c>, see <see cref="Tasks::Task::Schedule"/>), start it otherwise or report its exit
        /// yourself (<see cref="Tasks::Task::OnTaskExited"/> forwards to the group), or Wait never returns.
        /// </remarks>
        /// <seealso cref="_TaskGroup::Wait"/>
        void AddTask(ITask* task) noexcept
        {
            IncrementTaskCount();
            task->SetNotifyCallback(this);
        }

    protected:
        TaskGroupBase() noexcept = default;

        // A waiter that sees 0 sees the work of all tasks of the group. seq_cst on the decrement and
        // the read: the last exit decrements and then reads the waiter list, a waiter publishes
        // itself and then reads the count; with seq_cst at least one side sees the other (see
        // SuspendableBase<Single>::SetSuspendedTask), and on x64 it costs nothing extra.
        std::size_t GetTaskCount() noexcept { return taskCount_.load(std::memory_order_seq_cst); }
        std::size_t IncrementTaskCount() noexcept { return taskCount_.fetch_add(1, std::memory_order_relaxed) + 1; }
        std::size_t DecrementTaskCount() noexcept { return taskCount_.fetch_sub(1, std::memory_order_seq_cst) - 1; }

        // Calls that may still touch the group after a waiter could have passed: the exit
        // notifications and the waiter's own re-check after suspending. They enter before the count
        // can reach 0 (or before the waiter is published) and leave as their last access, so the
        // destructor only has to wait until none is inside.
        void EnterNotification() noexcept { notifying_.fetch_add(1, std::memory_order_relaxed); }
        void LeaveNotification() noexcept { notifying_.fetch_sub(1, std::memory_order_release); }

        void WaitForNotifications() const noexcept
        {
            // A notification takes no longer than one Resume; spin briefly, then give the thread
            // up in case the notifying thread was preempted.
            for (std::uint32_t spins = 0; notifying_.load(std::memory_order_acquire); ++spins) {
                if (spins < 64)
                    YieldProcessor();
                else
                    SwitchToThread();
            }
        }

    private:
        CT_ALIGNED std::atomic<std::size_t> taskCount_{0};
        // deliberately in taskCount_'s cache line: an exit touches both right after each other
        std::atomic<std::uint32_t> notifying_{0};
    };

    /// <summary>Waits for a group of tasks (fork/join).</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// <see cref="AddTask"/> counts a task in, its exit counts it out, and <see cref="Wait"/>
    /// passes once the count is <c>0</c>. Comes as <see cref="TaskGroup"/> (any number of waiters)
    /// and <see cref="TaskGroup_s"/> (one waiter; also Wait with a timeout).
    /// Wait suspends and needs a task or an OS thread converted with
    /// <see cref="ConvertThreadToTask"/>. The group is reusable: after the count reached 0, new
    /// tasks may be added for the next round, also by the task that just returned from Wait; a
    /// Wait of the next round passes only once that round's count is 0 again.
    /// The group may be a local object of the function that waits for it: once Wait has returned
    /// (or TryWait returned <c>true</c>) and no other task waits on it, it may be destroyed; the
    /// destructor waits for an exit notification that is still finishing. Destroy it only after all
    /// its tasks have exited.
    /// </remarks>
    /// <seealso cref="ITask"/>
    export template <SuspensionKind _SuspensionKind>
    class _TaskGroup : public TaskGroupBase, public Waitable<_SuspensionKind, _TaskGroup<_SuspensionKind>>
    {
    public:
        /// <summary>A resumed waiter looks at the count again (see <see cref="Waitable"/>).</summary>
        /// <remarks>
        /// The re-check after a suspension resumes every waiter on the list, which may include a
        /// waiter of the next round; that one finds the count above 0 and waits on.
        /// </remarks>
        static constexpr bool WaitRechecks = true;

        /// <summary>Creates an empty group.</summary>
        _TaskGroup() noexcept = default;

        /// <summary>Destroys the group.</summary>
        /// <remarks>Waits (briefly) until the exit notification of the last task has finished.</remarks>
        ~_TaskGroup() noexcept { WaitForNotifications(); }

        /// <summary>Counts an exited task out of the group.</summary>
        /// <remarks>
        /// Called when a task of the group exits (on its scheduler thread); the last one resumes
        /// the waiter(s). Application code does not call it.
        /// </remarks>
        void OnTaskExited() noexcept override
        {
            EnterNotification();
            if (!DecrementTaskCount())
                this->Resume();
            LeaveNotification();
        }

        /// <summary>Tells whether all tasks of the group have exited.</summary>
        /// <returns><c>true</c> if no task of the group is left (count <c>0</c>); otherwise <c>false</c>.</returns>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept { return !GetTaskCount(); }

    private:
        // ISuspendCallback: as Suspendable::OnSuspended (publish, then re-check), but inside a
        // notification: once published, the waiter may be resumed by the last task, return from
        // Wait and destroy the group while this re-check still runs.
        void OnSuspended(SuspendedTask suspendedTask) noexcept override
        {
            EnterNotification();
            if constexpr (_SuspensionKind == SuspensionKind::Multiple)
                this->AddSuspendedTask(suspendedTask);
            else
                this->SetSuspendedTask(suspendedTask);
            if (TryWait())
                this->Resume();
            LeaveNotification();
        }
    };

    /// <summary>Task group for any number of waiting tasks (no timeouts).</summary>
    /// <seealso cref="TaskGroup_s"/>
    export using TaskGroup = _TaskGroup<SuspensionKind::Multiple>;

    /// <summary>Task group for at most one waiting task.</summary>
    /// <remarks>Cheaper than <see cref="TaskGroup"/>; allows <see cref="Waitable::Wait"/> with a timeout.</remarks>
    /// <seealso cref="TaskGroup"/>
    export using TaskGroup_s = _TaskGroup<SuspensionKind::Single>;

    /// <summary>Matches the task group types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="TaskGroup"/> and <see cref="TaskGroup_s"/>.
    /// </remarks>
    /// <seealso cref="TaskGroup"/>
    /// <seealso cref="TaskGroup_s"/>
    export template <typename T>
    concept TaskGroupType = std::same_as<T, TaskGroup> || std::same_as<T, TaskGroup_s>;
} // namespace ThinkMeta::ConcurrentTasks::Synchronization