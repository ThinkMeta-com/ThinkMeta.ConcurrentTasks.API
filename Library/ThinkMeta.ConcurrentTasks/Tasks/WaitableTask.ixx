//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// WaitableTask: a Task that others can wait for until it has exited, built on a
// ManualResetEvent that OnTaskExited sets.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.SuspendableBase;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Tasks {
    using Synchronization::SuspensionKind;
    using Synchronization::_ManualResetEvent;

    /// <summary>A <see cref="Task"/> that others can wait for until it has exited.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// <see cref="Wait"/> suspends the caller until the task's <see cref="ITask::Run"/> has returned
    /// and <see cref="OnTaskExited"/> has been called. Derive from it and implement Run.
    /// <see cref="WaitableTask"/> allows any number of waiting tasks; <see cref="WaitableTask_s"/>
    /// allows at most one task waiting at a time, is cheaper and accepts a timeout. The waiting side
    /// must be a task or a thread converted with <see cref="ConvertThreadToTask"/>. After the first
    /// exit every Wait returns at once: an object is meant to run once.
    /// </remarks>
    /// <seealso cref="Synchronization::TaskGroup"/>
    template <SuspensionKind _SuspensionKind>
    class _WaitableTask : public Task
    {
    public:
        /// <summary>Suspends the calling task until this task has exited.</summary>
        /// <remarks>Returns at once if it has exited already.</remarks>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept { event_.Wait(); }

        /// <summary>Suspends the calling task until this task has exited, with a timeout.</summary>
        /// <param name="timeout">
        /// Longest time to wait, as a <see cref="Duration"/>; <see cref="Duration::Infinite"/> waits without
        /// a timeout.
        /// </param>
        /// <returns><c>true</c> if the task has exited; <c>false</c> if the wait timed out.</returns>
        /// <remarks><see cref="WaitableTask_s"/> only.</remarks>
        /// <seealso cref="TryWait"/>
        [[nodiscard]] bool Wait(Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            return event_.Wait(timeout);
        }

        /// <summary>Returns whether the task has exited, without waiting.</summary>
        /// <returns><c>true</c> if the task has exited; otherwise <c>false</c>.</returns>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept { return event_.TryWait(); }

        /// <summary>Called by the core when the task has exited.</summary>
        /// <remarks>
        /// Releases the waiting tasks, then tells the notify callback (e.g. a
        /// <see cref="Synchronization::TaskGroup"/>), if any. An override must call this version
        /// and must not touch the object after it.
        /// </remarks>
        void OnTaskExited() noexcept override
        {
            // A task waiting on this object may destroy it as soon as the event is set, and a task
            // waiting on the group may destroy it as soon as the group is told. So the callback is
            // read first, the event is set next, and the group is told last, without touching this
            // object again.
            auto* notifyTaskStateChange = GetNotifyCallback();
            event_.Set();
            if (notifyTaskStateChange)
                notifyTaskStateChange->OnTaskExited();
        }

    private:
        _ManualResetEvent<_SuspensionKind> event_;
    };

    /// <summary><see cref="_WaitableTask"/> for any number of waiting tasks.</summary>
    /// <seealso cref="WaitableTask_s"/>
    export using WaitableTask = _WaitableTask<SuspensionKind::Multiple>;

    /// <summary><see cref="_WaitableTask"/> for at most one waiting task at a time.</summary>
    /// <remarks>Cheaper, and <see cref="_WaitableTask::Wait"/> accepts a timeout.</remarks>
    /// <seealso cref="WaitableTask"/>
    export using WaitableTask_s = _WaitableTask<SuspensionKind::Single>;

    /// <summary>Matches the waitable task types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="WaitableTask"/> and <see cref="WaitableTask_s"/>.
    /// </remarks>
    /// <seealso cref="WaitableTask"/>
    /// <seealso cref="WaitableTask_s"/>
    export template <typename T>
    concept WaitableTaskType = std::same_as<T, WaitableTask> || std::same_as<T, WaitableTask_s>;
} // namespace ThinkMeta::ConcurrentTasks::Tasks