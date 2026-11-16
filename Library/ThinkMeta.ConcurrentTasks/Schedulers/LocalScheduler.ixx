//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// LocalScheduler: owns a local scheduler of the core (CreateLocalScheduler), whose tasks run inside
// Run on whatever calls it - the main thread, say, which works along instead of only waiting.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Schedulers.LocalScheduler;

namespace ThinkMeta::ConcurrentTasks::Schedulers {
    /// <summary>Owns a scheduler whose tasks run on the thread that calls <see cref="Run"/>.</summary>
    /// <remarks>
    /// Add tasks, then call <see cref="Run"/>: it runs them on the calling thread and returns once
    /// every task of the scheduler has ended, including those the tasks add and those started by
    /// time. While all of them wait, the thread sleeps. The tasks never run on another thread.
    ///
    /// The thread that calls Run owns the scheduler: only it adds tasks, before Run or from the
    /// scheduler's tasks. Tasks may be resumed from any thread, so they can hand work to another
    /// scheduler and wait for it
    /// (<see cref="Tasks::Task::Schedule(ITaskScheduler&amp;, TaskPriority)"/>,
    /// <see cref="Tasks::RunAndWait"/>, <see cref="Synchronization::TaskGroup_s"/>), sleep and wait
    /// with a timeout. The order is that of the default scheduler.
    ///
    /// Usage rules: call Run on a plain thread, never from a task or on a converted thread. Create
    /// and destroy the scheduler on that thread; it may be destroyed as soon as Run has returned,
    /// but never by one of its tasks. A task that waits for something only the caller of Run can
    /// provide waits for ever. Performance is not this scheduler's aim; it is about where tasks run.
    /// </remarks>
    /// <seealso cref="ILocalScheduler"/>
    export class LocalScheduler
    {
    public:
        /// <summary>Creates the scheduler, on the thread that will call <see cref="Run"/>.</summary>
        /// <param name="taskStackSize">Stack size per task in bytes; <c>0</c> is the platform's default (see <see cref="SchedulerInfo::taskStackSize"/>).</param>
        /// <param name="taskQueueCapacity">Capacity of each task queue; <c>0</c> is <see cref="DefaultTaskQueueCapacity"/> (see <see cref="SchedulerInfo::taskQueueCapacity"/>).</param>
        explicit LocalScheduler(std::uint32_t taskStackSize = 0, std::uint32_t taskQueueCapacity = 0) noexcept
        {
            const SchedulerInfo info{.taskQueueCapacity = taskQueueCapacity, .taskStackSize = taskStackSize};
            scheduler_ = CreateLocalScheduler(&info);
        }

        LocalScheduler(const LocalScheduler&)            = delete;
        LocalScheduler(LocalScheduler&&)                 = delete;
        LocalScheduler& operator=(const LocalScheduler&) = delete;
        LocalScheduler& operator=(LocalScheduler&&)      = delete;

        /// <summary>Destroys the scheduler.</summary>
        /// <remarks>Only after <see cref="Run"/> has returned and no task is left.</remarks>
        ~LocalScheduler()
        {
            if (scheduler_)
                scheduler_->Destroy();
        }

        /// <summary>Returns whether the scheduler was created.</summary>
        explicit operator bool() const noexcept { return scheduler_ != nullptr; }

        /// <summary>Returns the core's scheduler.</summary>
        ILocalScheduler* Get() const noexcept { return scheduler_; }

        /// <summary>Returns the core's scheduler.</summary>
        ILocalScheduler* operator->() const noexcept { return scheduler_; }

        /// <summary>Returns the scheduler as a target for tasks, e.g. for <see cref="Tasks::Task::Schedule(ITaskScheduler&amp;, TaskPriority)"/>.</summary>
        operator ITaskScheduler&() const noexcept { return *scheduler_; }

        /// <summary>Adds a task; it runs in the next or the current <see cref="Run"/>.</summary>
        /// <param name="task">The task; must stay valid until it has exited.</param>
        /// <param name="priority"><see cref="TaskPriority::Default"/> inherits the calling task's priority.</param>
        /// <remarks>Only on the thread that calls <see cref="Run"/>: before Run, or from a task of this scheduler.</remarks>
        /// <seealso cref="ITaskScheduler::AddTask"/>
        void AddTask(ITask* task, TaskPriority priority = TaskPriority::Default) noexcept { scheduler_->AddTask(task, priority); }

        /// <summary>Runs the tasks on the calling thread and blocks it until none is left.</summary>
        /// <remarks>Only on a plain thread, never from a task or on a converted thread.</remarks>
        /// <seealso cref="ILocalScheduler::Run"/>
        void Run() noexcept { scheduler_->Run(); }

    private:
        ILocalScheduler* scheduler_ = nullptr;
    };
} // namespace ThinkMeta::ConcurrentTasks::Schedulers
