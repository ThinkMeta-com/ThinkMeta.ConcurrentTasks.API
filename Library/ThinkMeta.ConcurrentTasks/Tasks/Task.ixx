//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Task: the library's base class for tasks (an ITask with timer storage and an exit
// notification), and LambdaTask, a Task that runs a callable. Task::Create builds one.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <concepts>
#include <type_traits>
#include <utility>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Tasks {
    template <std::invocable F>
    class LambdaTask;

    /// <summary>The library's base class for tasks.</summary>
    /// <remarks>
    /// Derive from it and implement <see cref="ITask::Run"/>. It can be started at once or by time,
    /// and tells an optional <see cref="INotifyTaskStateChange"/> when it has exited.
    /// The object is owned by the caller and must outlive the task: from <see cref="Schedule"/>
    /// until <see cref="OnTaskExited"/> has been called (e.g. wait for it with a
    /// <see cref="WaitableTask"/> or a <see cref="Synchronization::TaskGroup"/>).
    /// </remarks>
    /// <seealso cref="LambdaTask"/>
    export class CT_NOVTABLE Task : public ITask
    {
        // Not copyable or movable: the scheduler holds the task's address.
        Task(const Task&)            = delete;
        Task(Task&&)                 = delete;
        Task& operator=(const Task&) = delete;
        Task& operator=(Task&&)      = delete;

    public:
        // INotifyTaskStateChange

        /// <summary>Called by the core when the task has exited.</summary>
        /// <remarks>
        /// Tells the notify callback, if any. From then on the notified party may destroy or reuse
        /// the task object.
        /// </remarks>
        /// <seealso cref="SetNotifyCallback"/>
        // An override must tell GetNotifyCallback() as its last action and must not touch the
        // object after it (see _WaitableTask::OnTaskExited).
        void OnTaskExited() noexcept override
        {
            if (notifyTaskStateChange_)
                notifyTaskStateChange_->OnTaskExited();
        }

        // ITask

        /// <summary>Sets the object <see cref="OnTaskExited"/> forwards to.</summary>
        /// <param name="notifyTaskStateChange">
        /// The observer, borrowed: it must stay valid until the task has exited. <c>nullptr</c>: none.
        /// </param>
        /// <remarks>
        /// Replaces the one given to the constructor (<see cref="Synchronization::TaskGroup"/> uses it
        /// to count its tasks). Set it before the task is scheduled.
        /// </remarks>
        void SetNotifyCallback(INotifyTaskStateChange* notifyTaskStateChange) noexcept override { notifyTaskStateChange_ = notifyTaskStateChange; }

        /// <summary>Returns the task's own timer storage.</summary>
        /// <returns>Storage inside this object; never <c>nullptr</c>.</returns>
        /// <remarks>So <see cref="Task"/> objects can be started by time.</remarks>
        TaskTimerData* GetTimerData() noexcept override { return &timerData_; }

        /// <summary>Adds the task to the calling task's scheduler.</summary>
        /// <param name="taskPriority">
        /// Priority the task runs with; <see cref="TaskPriority::Default"/> inherits the calling task's
        /// priority.
        /// </param>
        /// <remarks>
        /// Uses <see cref="ITaskScheduler::AddTask"/>. Call it from a task running on a scheduler;
        /// from a plain or converted thread call AddTask on the scheduler object instead.
        /// </remarks>
        /// <seealso cref="CancelSchedule"/>
        void Schedule(TaskPriority taskPriority = TaskPriority::Default) noexcept { GetTaskScheduler()->AddTask(this, taskPriority); }

        /// <summary>Adds the task to the given scheduler.</summary>
        /// <param name="scheduler">The scheduler to run the task on.</param>
        /// <param name="taskPriority">
        /// Priority the task runs with; <see cref="TaskPriority::Default"/> inherits the calling task's
        /// priority.
        /// </param>
        /// <remarks>
        /// Uses <see cref="ITaskScheduler::AddTask"/>; any thread or task. Use it to hand work to
        /// another scheduler, e.g. from a task of a dispatcher scheduler (a UI event handler) to the
        /// worker threads of a default scheduler: there the calling task's own scheduler, which
        /// <see cref="Schedule(TaskPriority)"/> uses, is the UI thread.
        /// </remarks>
        void Schedule(ITaskScheduler& scheduler, TaskPriority taskPriority = TaskPriority::Default) noexcept { scheduler.AddTask(this, taskPriority); }

        /// <summary>Adds the task to the calling task's scheduler after a delay.</summary>
        /// <param name="taskPriority">
        /// Priority the task runs with; <see cref="TaskPriority::Default"/> inherits the calling task's
        /// priority.
        /// </param>
        /// <param name="delay">
        /// Time from now until the task is queued, as a <see cref="Duration"/> (e.g.
        /// <see cref="Duration::Milliseconds"/>).
        /// </param>
        /// <returns>
        /// <c>true</c> if the task is waiting to start; <c>false</c> if the task cannot be started by time (see
        /// <see cref="ITaskScheduler::AddTimedTask"/>).
        /// </returns>
        /// <remarks>
        /// Needs <see cref="EnableTimers"/>. Called again before the task has started, it moves the
        /// start.
        /// On <c>false</c> the task was not added: it neither runs nor exits, so
        /// <see cref="OnTaskExited"/> is not called. Usage rule: a task already counted into a
        /// <see cref="Synchronization::TaskGroup"/> stays counted and the group's Wait never returns,
        /// unless you start it without delay (<see cref="Schedule"/> with the priority only) or report its
        /// exit yourself by calling its <see cref="OnTaskExited"/>, which counts it out of the group.
        /// </remarks>
        /// <seealso cref="CancelSchedule"/>
        /// <seealso cref="ScheduleNow"/>
        bool Schedule(TaskPriority taskPriority, Duration delay) noexcept { return GetTaskScheduler()->AddTimedTask(this, taskPriority, delay); }

        /// <summary>Takes back a start by time that has not happened yet.</summary>
        /// <returns>
        /// <c>true</c> if the start was taken back; <c>false</c> if the task had started already or was
        /// not started by time.
        /// </returns>
        /// <remarks>
        /// The task exits without running (<see cref="OnTaskExited"/>). Never blocks.
        /// </remarks>
        /// <seealso cref="CancelTimedTask"/>
        bool CancelSchedule() noexcept { return CancelTimedTask(this); }

        /// <summary>Starts a task scheduled by time now instead of later.</summary>
        /// <returns><c>true</c> if the task was queued now; <c>false</c> if it was not waiting to start.</returns>
        /// <remarks>For a task started by <see cref="Schedule"/> with a delay.</remarks>
        /// <seealso cref="ScheduleTimedTask"/>
        bool ScheduleNow() noexcept { return ScheduleTimedTask(this); }

        /// <summary>Lets the calling task sleep.</summary>
        /// <param name="duration">
        /// Minimum time to sleep, as a <see cref="Duration"/> (e.g. <see cref="Duration::Milliseconds"/>).
        /// </param>
        /// <returns><c>true</c> after the sleep; <c>false</c> at once without <see cref="EnableTimers"/>.</returns>
        /// <remarks>The thread runs other tasks meanwhile.</remarks>
        /// <seealso cref="SleepTask"/>
        // Not named Sleep, so that it does not hide ::Sleep in classes derived from Task.
        static bool SleepFor(Duration duration) noexcept { return SleepTask(duration); }

        /// <summary>Returns a <see cref="LambdaTask"/> that runs a callable.</summary>
        /// <typeparam name="F">
        /// Type of the callable: invocable without arguments; it must not throw.
        /// </typeparam>
        /// <param name="func">The callable; stored by value (copied or moved).</param>
        /// <param name="notifyTaskStateChange">
        /// Told when the task exits; borrowed, must stay valid until then. May be <c>nullptr</c>.
        /// </param>
        /// <returns>The unscheduled task, a <see cref="LambdaTask"/> of the decayed callable type.</returns>
        /// <remarks>
        /// The task is returned by value and is not scheduled yet: keep it in a variable that
        /// outlives the task and call <see cref="Schedule"/> or <see cref="ITaskScheduler::AddTask"/>
        /// on it. References captured by func must stay valid until the task has exited.
        /// </remarks>
        template <std::invocable F>
        static auto Create(F&& func, INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept
        {
            using DecayedF = std::decay_t<F>;
            return LambdaTask<DecayedF>(std::forward<F>(func), notifyTaskStateChange);
        }

    protected:
        /// <summary>Constructs the task.</summary>
        /// <param name="notifyTaskStateChange">Told when the task exits; may be <c>nullptr</c>.</param>
        explicit Task(INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept : notifyTaskStateChange_(notifyTaskStateChange) {}

        /// <summary>Returns the object that is told when the task exits.</summary>
        /// <returns>The notify callback, or <c>nullptr</c> if none is set.</returns>
        /// <remarks>
        /// For an <see cref="OnTaskExited"/> override that must not touch the task any more after
        /// releasing its own waiters: read the callback first, then release, then notify.
        /// </remarks>
        INotifyTaskStateChange* GetNotifyCallback() const noexcept { return notifyTaskStateChange_; }

    private:
        INotifyTaskStateChange* notifyTaskStateChange_;
        TaskTimerData           timerData_{};
    };

    /// <summary>A <see cref="Task"/> that runs a stored callable.</summary>
    /// <typeparam name="F">
    /// Type of the stored callable: an object type invocable without arguments; it must not throw.
    /// </typeparam>
    /// <remarks>
    /// <see cref="Run"/> calls the stored callable of type <typeparamref name="F"/>. Usually built by
    /// <see cref="Task::Create"/>. The callable must not throw: an exception escaping it ends the
    /// process.
    /// </remarks>
    export template <std::invocable F>
    class LambdaTask : public Task
    {
    public:
        /// <summary>Stores the callable.</summary>
        /// <typeparam name="FF">Type of the argument; <typeparamref name="F"/> must be constructible from it.</typeparam>
        /// <param name="func">Copied or moved into <typeparamref name="F"/>.</param>
        /// <param name="notifyTaskStateChange">Told when the task exits; may be <c>nullptr</c>.</param>
        template <typename FF>
            requires std::constructible_from<F, FF&&>
        explicit LambdaTask(FF&& func, INotifyTaskStateChange* notifyTaskStateChange = nullptr) noexcept : Task(notifyTaskStateChange), func_(std::forward<FF>(func)) {}

        /// <summary>Calls the stored callable.</summary>
        /// <remarks>The task may be run again after <see cref="Task::OnTaskExited"/>.</remarks>
        void Run() noexcept override { func_(); }

    private:
        F func_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Tasks