//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// RunAndWait: runs a callable as a task on another scheduler and lets the calling task wait for
// it - the usual move of a UI event handler (a task of a dispatcher scheduler) that hands CPU work
// to the worker threads of a default scheduler.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Tasks.RunAndWait;

import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace ThinkMeta::ConcurrentTasks::Tasks {
    /// <summary>Runs a callable on a scheduler and waits until it has finished.</summary>
    /// <typeparam name="F">Type of the callable: invocable without arguments; it must not throw.</typeparam>
    /// <param name="scheduler">The scheduler to run the callable on, e.g. the worker threads.</param>
    /// <param name="func">The callable; neither copied nor moved, since the call waits for it.</param>
    /// <remarks>
    /// The calling task waits without blocking its thread: called from a task of a dispatcher scheduler
    /// (an event handler on the UI thread), the UI keeps running and the handler continues on the UI
    /// thread once the callable has finished. Task or converted thread only.
    /// To spread work over several tasks, count them into a <see cref="Synchronization::TaskGroup_s"/>,
    /// add them with <see cref="Task::Schedule(ITaskScheduler&amp;, TaskPriority)"/> and wait for the group.
    /// </remarks>
    /// <seealso cref="WaitableLambdaTask_s"/>
    export template <std::invocable F>
    void RunAndWait(ITaskScheduler& scheduler, F&& func) noexcept
    {
        // A single waiter: the task may go as soon as Wait has returned.
        auto task = WaitableTask_s::Create([&func]() noexcept { func(); });
        task.Schedule(scheduler);
        task.Wait();
    }
} // namespace ThinkMeta::ConcurrentTasks::Tasks
