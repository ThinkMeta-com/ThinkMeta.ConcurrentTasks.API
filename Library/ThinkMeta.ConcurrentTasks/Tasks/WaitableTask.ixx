//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// WaitableTask: a Task that others can wait for until it has exited, built on a
// ManualResetEvent that OnTaskExited sets; WaitableLambdaTask, one that runs a callable.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <concepts>
#include <type_traits>
#include <utility>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.SuspendableBase;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Tasks {
    using Synchronization::SuspensionKind;
    using Synchronization::_ManualResetEvent;

    template <SuspensionKind _SuspensionKind, std::invocable F>
    class _WaitableLambdaTask;

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
    /// Lifetime: with <see cref="WaitableTask_s"/> the waiting task may destroy the object as soon as
    /// its Wait returned, since the exit's Set is one operation. With <see cref="WaitableTask"/> it
    /// may not: the exit stores the signal and then takes the list of waiters, and a waiter that
    /// finds the signal passes in between (see <see cref="Synchronization::ManualResetEvent"/>);
    /// destroy it only when no task waits on it any more and the exit has returned - or wait for
    /// the task through a <see cref="Synchronization::TaskGroup"/>, which covers this case.
    /// </remarks>
    /// <seealso cref="Synchronization::TaskGroup"/>
    export template <SuspensionKind _SuspensionKind>
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
            // A task waiting on this object may destroy it as soon as the event is set (the Single
            // variant; the Multiple one must stay until Set has returned), and a task waiting on
            // the group may destroy it as soon as the group is told. So the callback is read first,
            // the event is set next, and the group is told last, without touching this object again.
            auto* notifyTaskStateChange = GetNotifyCallback();
            event_.Set();
            if (notifyTaskStateChange)
                notifyTaskStateChange->OnTaskExited();
        }

        /// <summary>Returns a waitable task that runs a callable.</summary>
        /// <typeparam name="F">
        /// Type of the callable: invocable without arguments; it must not throw.
        /// </typeparam>
        /// <param name="func">The callable; stored by value (copied or moved).</param>
        /// <param name="notifyTaskStateChange">
        /// Told when the task exits; borrowed, must stay valid until then. May be <c>nullptr</c>.
        /// </param>
        /// <returns>
        /// The unscheduled task, a <see cref="_WaitableLambdaTask"/> of this suspension kind and the
        /// decayed callable type.
        /// </returns>
        /// <remarks>
        /// Hides <see cref="Task::Create"/>, which returns a task that cannot be waited for. Keep the
        /// task in a variable that outlives it, schedule it and wait for it:
        /// <c>auto task = WaitableTask_s::Create(f); task.Schedule(scheduler); task.Wait();</c>
        /// References captured by func must stay valid until the task has exited.
        /// </remarks>
        template <std::invocable F>
        static auto Create(F&& func, INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept
        {
            return _WaitableLambdaTask<_SuspensionKind, std::decay_t<F>>(std::forward<F>(func), notifyTaskStateChange);
        }

    protected:
        /// <summary>Constructs the task.</summary>
        /// <param name="notifyTaskStateChange">Told when the task exits; may be <c>nullptr</c>.</param>
        explicit _WaitableTask(INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept : Task(notifyTaskStateChange) {}

    private:
        _ManualResetEvent<_SuspensionKind> event_;
    };

    /// <summary>A waitable task that runs a stored callable.</summary>
    /// <typeparam name="_SuspensionKind">As for <see cref="_WaitableTask"/>.</typeparam>
    /// <typeparam name="F">
    /// Type of the stored callable: an object type invocable without arguments; it must not throw.
    /// </typeparam>
    /// <remarks>
    /// The waitable counterpart of <see cref="LambdaTask"/>. Usually built by
    /// <see cref="_WaitableTask::Create"/>; comes as <see cref="WaitableLambdaTask"/> and
    /// <see cref="WaitableLambdaTask_s"/>. The callable must not throw: an exception escaping it ends
    /// the process.
    /// </remarks>
    export template <SuspensionKind _SuspensionKind, std::invocable F>
    class _WaitableLambdaTask : public _WaitableTask<_SuspensionKind>
    {
    public:
        /// <summary>Stores the callable.</summary>
        /// <typeparam name="FF">Type of the argument; <typeparamref name="F"/> must be constructible from it.</typeparam>
        /// <param name="func">Copied or moved into <typeparamref name="F"/>.</param>
        /// <param name="notifyTaskStateChange">Told when the task exits; may be <c>nullptr</c>.</param>
        template <typename FF>
            requires std::constructible_from<F, FF&&>
        explicit _WaitableLambdaTask(FF&& func, INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept
            : _WaitableTask<_SuspensionKind>(notifyTaskStateChange), func_(std::forward<FF>(func))
        {
        }

        /// <summary>Calls the stored callable.</summary>
        void Run() noexcept override { func_(); }

    private:
        F func_;
    };

    /// <summary><see cref="_WaitableTask"/> for any number of waiting tasks.</summary>
    /// <seealso cref="WaitableTask_s"/>
    export using WaitableTask = _WaitableTask<SuspensionKind::Multiple>;

    /// <summary><see cref="_WaitableTask"/> for at most one waiting task at a time.</summary>
    /// <remarks>Cheaper, and <see cref="_WaitableTask::Wait"/> accepts a timeout.</remarks>
    /// <seealso cref="WaitableTask"/>
    export using WaitableTask_s = _WaitableTask<SuspensionKind::Single>;

    /// <summary><see cref="_WaitableLambdaTask"/> for any number of waiting tasks.</summary>
    /// <typeparam name="F">Type of the stored callable.</typeparam>
    export template <std::invocable F>
    using WaitableLambdaTask = _WaitableLambdaTask<SuspensionKind::Multiple, F>;

    /// <summary><see cref="_WaitableLambdaTask"/> for at most one waiting task at a time.</summary>
    /// <typeparam name="F">Type of the stored callable.</typeparam>
    /// <remarks>Cheaper, and Wait accepts a timeout; the waiter may destroy it as soon as Wait returned.</remarks>
    export template <std::invocable F>
    using WaitableLambdaTask_s = _WaitableLambdaTask<SuspensionKind::Single, F>;

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