//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The ABI of the ThinkMeta.ConcurrentTasks core: the scheduler's C entry points and the
// interfaces exchanged with it. This header is the only thing a caller needs in order to talk
// to the core; everything above it (tasks, synchronization primitives, collections, async I/O)
// is ordinary C++ built on top of these declarations.
//
// The entry points have C language linkage, so their exported names are undecorated and the
// surrounding namespace exists for C++ callers only.
//
// Preliminary API: everything in this file is subject to change.
//
// Performance first: the core and the library are built for speed, not to be watertight against
// misuse. Correct use - lifetimes of objects tasks wait on, queue capacities, single-waiter rules -
// is the caller's responsibility; debug builds assert some of it, release builds check nothing.
//

#pragma once

#include <cstdint>

// The platform: the scheduler is the same everywhere, only the asynchronous I/O part at the end of
// this header and the native thread type are the platform's own. Guarded because the library's
// Defs.h defines the same macros.
#if !defined(CT_PLATFORM_WINDOWS) && !defined(CT_PLATFORM_LINUX)
#    if defined(_WIN32)
#        define CT_PLATFORM_WINDOWS 1
#    elif defined(__linux__)
#        define CT_PLATFORM_LINUX 1
#    else
#        error "ThinkMeta.ConcurrentTasks: unsupported platform"
#    endif
#endif

#if defined(CT_PLATFORM_WINDOWS)
// Without the min and max macros of Windows.h: they break std::min and std::max in every file that
// includes Core.h. A file that includes Windows.h before Core.h decides this itself.
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#else
#    include <pthread.h>
#endif

// A path literal as the platform's file functions take it: UTF-16 (L"...") on Windows, UTF-8
// elsewhere; the library's Platform::FileName is the matching type. CT_PATH("out.bin").
#ifndef CT_PATH
#    if defined(CT_PLATFORM_WINDOWS)
#        define CT_PATH(text) L##text
#    else
#        define CT_PATH(text) text
#    endif
#endif

// Marks the pure interfaces below: they are never instantiated themselves, so with MSVC their
// constructors need not set up a vtable of their own. Guarded because the library's Defs.h defines
// it too.
#ifndef CT_NOVTABLE
#    if defined(_MSC_VER) && !defined(__clang__)
#        define CT_NOVTABLE __declspec(novtable)
#    else
#        define CT_NOVTABLE
#    endif
#endif

// The core's entry points are exported from the core binary (ThinkMeta.ConcurrentTasks.Core.dll,
// libThinkMeta.ConcurrentTasks.Core.so.2). A caller gets them as imports; the core itself defines
// CT_CORE_EXPORTS, and CT_CORE_STATIC covers the case of linking the core's sources directly.
#if defined(CT_CORE_STATIC)
#    define CT_CORE
#elif defined(CT_PLATFORM_WINDOWS)
#    if defined(CT_CORE_EXPORTS)
#        define CT_CORE __declspec(dllexport)
#    else
#        define CT_CORE __declspec(dllimport)
#    endif
#else
#    define CT_CORE __attribute__((visibility("default")))
#endif

namespace ThinkMeta::ConcurrentTasks {
    /// <summary>ABI version this header describes.</summary>
    /// <remarks>
    /// Passed in <see cref="SchedulerInfo::version"/>; the core accepts only its own version.
    /// </remarks>
    /// <seealso cref="CreateTaskScheduler"/>
    inline constexpr std::uint32_t SchedulerVersion = 2;

    /// <summary>The handle of a native thread: <c>HANDLE</c> on Windows, <c>pthread_t</c> elsewhere.</summary>
    /// <seealso cref="ThreadCallbackProc"/>
#if defined(CT_PLATFORM_WINDOWS)
    using NativeThread = void*;
#else
    using NativeThread = pthread_t;
#endif

    /// <summary>A span of time, counted in units of 100 nanoseconds.</summary>
    /// <remarks>
    /// 100 ns is the unit of Windows timers. Built with <see cref="Duration::Microseconds"/>,
    /// <see cref="Duration::Milliseconds"/> or <see cref="Duration::Seconds"/>, so that a plain number
    /// never passes for one.
    /// The core's timer thread waits on a high-resolution timer: on Windows 10 1803 and later it
    /// fires on a grid of about half a millisecond, on older Windows on the system tick (about
    /// 15.6 ms), on Linux on the kernel's high-resolution timers. A duration says how long at
    /// least; the task may start or wake that much later.
    /// </remarks>
    /// <seealso cref="Duration::Infinite"/>
    struct Duration {
        /// <summary>The length in units of 100 nanoseconds.</summary>
        std::int64_t hundredNanoseconds;

        /// <summary>A duration of microseconds.</summary>
        /// <param name="count">Number of microseconds; small enough not to overflow.</param>
        /// <returns>The duration of <paramref name="count"/> microseconds.</returns>
        static constexpr Duration Microseconds(std::int64_t count) noexcept { return {count * 10}; }

        /// <summary>A duration of milliseconds.</summary>
        /// <param name="count">Number of milliseconds; small enough not to overflow.</param>
        /// <returns>The duration of <paramref name="count"/> milliseconds.</returns>
        static constexpr Duration Milliseconds(std::int64_t count) noexcept { return {count * 10000}; }

        /// <summary>A duration of seconds.</summary>
        /// <param name="count">Number of seconds; small enough not to overflow.</param>
        /// <returns>The duration of <paramref name="count"/> seconds.</returns>
        static constexpr Duration Seconds(std::int64_t count) noexcept { return {count * 10000000}; }

        /// <summary>A timeout that never expires.</summary>
        /// <returns>The largest representable duration, reserved as "no timeout".</returns>
        /// <remarks>Waiting with it is waiting without a timeout.</remarks>
        /// <seealso cref="IsInfinite"/>
        static constexpr Duration Infinite() noexcept { return {0x7FFFFFFFFFFFFFFFLL}; }

        /// <summary>Returns whether this is <see cref="Infinite"/>.</summary>
        /// <returns><c>true</c> if this duration equals <see cref="Infinite"/>; otherwise <c>false</c>.</returns>
        /// <remarks>Exactly; no other value counts.</remarks>
        constexpr bool IsInfinite() const noexcept { return hundredNanoseconds == Infinite().hundredNanoseconds; }
    };

    /// <summary>Default task queue capacity.</summary>
    /// <remarks>Used when <see cref="SchedulerInfo::taskQueueCapacity"/> is <c>0</c>.</remarks>
    inline constexpr std::uint32_t DefaultTaskQueueCapacity = 4096;

    /// <summary>Largest task queue capacity <see cref="CreateTaskScheduler"/> accepts.</summary>
    inline constexpr std::uint32_t MaximumTaskQueueCapacity = std::uint32_t{1} << 20;

    /// <summary>An entry of an intrusive singly linked list: the link lives in the item.</summary>
    /// <remarks>
    /// Items of <see cref="Collections::BasicStack"/> and <see cref="Collections::ConcurrentStack"/>
    /// derive from it; whoever holds an item on a list owns its <see cref="next"/>. Aligned to 16
    /// bytes, as the lock-free stack requires; on Windows it has the layout of <c>SLIST_ENTRY</c>.
    /// </remarks>
    /// <seealso cref="TaskEntry"/>
    struct alignas(16) ListEntry {
        /// <summary>The next item on the list, or <c>nullptr</c> at its end.</summary>
        ListEntry* next;
    };

    /// <summary>The scheduler-owned bookkeeping of one task.</summary>
    /// <remarks>
    /// Addressed as an opaque handle (<see cref="TaskHandle"/>). The layout is private to the core.
    /// </remarks>
    /// <seealso cref="TaskHandle"/>
    struct TaskEntry : ListEntry {};

    /// <summary>Handle of a task.</summary>
    /// <remarks>
    /// Identifies a task, running or not (<see cref="GetCurrentTask"/>, <see cref="GetTask"/>). Only a
    /// <see cref="SuspendedTask"/> can be resumed.
    /// </remarks>
    /// <seealso cref="SuspendedTask"/>
    using TaskHandle = TaskEntry*;

    /// <summary>The bookkeeping of a task while it is suspended.</summary>
    /// <remarks>
    /// The same object as its <see cref="TaskEntry"/>, typed for the suspended state. The leading
    /// <see cref="ListEntry"/> belongs to whoever suspended the task: a synchronization primitive may
    /// chain suspended tasks through it (see <see cref="Collections::BasicStack"/> and
    /// <see cref="Collections::ConcurrentStack"/>) from the moment
    /// <see cref="ISuspendCallback::OnSuspended"/> hands it over until it passes it back to
    /// <see cref="ResumeTask"/>. Outside that window the field belongs to the core.
    /// </remarks>
    /// <seealso cref="SuspendedTask"/>
    struct SuspendedTaskEntry : TaskEntry {};

    /// <summary>Handle of a suspended task: the only kind of handle <see cref="ResumeTask"/> takes.</summary>
    /// <remarks>
    /// Handed out by <see cref="ISuspendCallback::OnSuspended"/> (and
    /// <see cref="ISuspendCallback::OnWithdraw"/>); valid for exactly one <see cref="ResumeTask"/>. It
    /// converts to a <see cref="TaskHandle"/>, but a TaskHandle - for example the calling task's own
    /// - does not convert to it, so a running task cannot be resumed by mistake.
    /// </remarks>
    /// <seealso cref="SuspendTask"/>
    /// <seealso cref="ResumeTask"/>
    using SuspendedTask = SuspendedTaskEntry*;

    /// <summary>Tells how a wait ended.</summary>
    /// <seealso cref="SuspendTaskFor"/>
    enum class WaitStatus : std::int32_t {
        /// <summary>A <see cref="ResumeTask"/> ended the wait: the condition came true.</summary>
        Signaled,

        /// <summary>The timeout struck first.</summary>
        TimedOut,

        /// <summary>The task was asked to cancel (<see cref="CancelTask"/>).</summary>
        Cancelled
    };

    /// <summary>Selects how <see cref="YieldTask"/> gives up the processor.</summary>
    /// <remarks>
    /// Default scheduler only; <see cref="SchedulerKind::TaskSwap"/> and converted threads ignore
    /// the flag.
    /// </remarks>
    enum class YieldFlags : std::int32_t {
        /// <summary>Lets the next runnable task run.</summary>
        /// <remarks>
        /// The task goes back to the end of its priority's queue; if nothing else is runnable, it
        /// runs again at once.
        /// </remarks>
        Default,

        /// <summary>Yields only to waiting <see cref="TaskPriority::High"/> work.</summary>
        /// <remarks>
        /// If there is no High task or resumed High task context, or the caller is High itself,
        /// <see cref="YieldTask"/> returns at once.
        /// </remarks>
        HigherPriority
    };

    /// <summary>Priority of a task.</summary>
    /// <remarks>
    /// The Default scheduler keeps one queue pair per real priority; <see cref="SchedulerKind::TaskSwap"/>
    /// ignores priorities.
    /// </remarks>
    /// <seealso cref="SetTaskPriority"/>
    enum class TaskPriority : std::int32_t {
        /// <summary>Inherits the calling task's priority.</summary>
        /// <remarks>
        /// For <see cref="ITaskScheduler::AddTask"/>: the calling task's priority, or
        /// <see cref="Normal"/> when the caller is not a task. Not meaningful as a task's own priority.
        /// </remarks>
        Default,

        /// <summary>Runs when no <see cref="High"/> work is runnable.</summary>
        Normal,

        /// <summary>Always taken before <see cref="Normal"/> work.</summary>
        /// <remarks>There is no aging: a steady stream of High work starves Normal tasks.</remarks>
        High
    };

    /// <summary>Tells a <see cref="ThreadCallbackProc"/> why it is called.</summary>
    enum class ThreadCallbackReason : std::int32_t {
        /// <summary>A new scheduler thread starts.</summary>
        /// <remarks>Called on that thread before it runs its first task.</remarks>
        Starting,

        /// <summary>A scheduler thread ends.</summary>
        /// <remarks>
        /// Called on that thread after it has left the scheduling loop (after
        /// <see cref="ITaskScheduler::Stop"/>), right before the thread ends.
        /// </remarks>
        Exiting
    };

    /// <summary>Selects the scheduler implementation.</summary>
    /// <seealso cref="CreateTaskScheduler"/>
    enum class SchedulerKind : std::int32_t {
        /// <summary>Work-stealing scheduler over several threads.</summary>
        /// <remarks>The thread count is <see cref="SchedulerInfo::numberOfThreads"/>.</remarks>
        Default,

        /// <summary>Single-threaded scheduler swapping two tasks under a switch policy.</summary>
        /// <remarks>The policy is <see cref="SchedulerInfo::taskSwitchPolicy"/>.</remarks>
        /// <seealso cref="ITaskSwitchPolicy"/>
        TaskSwap
    };

    struct ITaskScheduler;

    /// <summary>Receives the handle of the task that has just been suspended.</summary>
    /// <seealso cref="SuspendTask"/>
    /// <seealso cref="SuspendTaskFor"/>
    struct CT_NOVTABLE ISuspendCallback {
        /// <summary>Called once the suspending task is off the processor.</summary>
        /// <param name="suspendedTask">
        /// The task's handle. The waiter publishes it here (e.g. pushes it onto its wait list) or, if
        /// the condition it waits for has already come true, passes it straight to
        /// <see cref="ResumeTask"/>.
        /// </param>
        /// <remarks>
        /// Runs on the scheduler thread, outside any task (for a converted thread: on that thread,
        /// before it blocks), so it must not suspend, yield or wait. From the moment the handle is
        /// published a resumer on another thread may resume the task, so the callback must not
        /// touch the waiter's state after publishing unless it knows the task cannot run yet.
        /// </remarks>
        /// <seealso cref="OnWithdraw"/>
        virtual void OnSuspended(SuspendedTask suspendedTask) noexcept = 0;

        /// <summary>Asks the waiter to take a suspended task back, so the core can end its wait early.</summary>
        /// <param name="suspendedTask">
        /// The handle <see cref="OnSuspended"/> received for this suspension; the waiter removes it from
        /// where it was published if it is still there.
        /// </param>
        /// <returns>
        /// <c>true</c> if the waiter took the task back from wherever <see cref="OnSuspended"/>
        /// published it: the core then resumes it with the reason it ended the wait for. <c>false</c>
        /// if a resumer took the task first; the core then waits for that resumer's
        /// <see cref="ResumeTask"/>.
        /// </returns>
        /// <remarks>
        /// Called while the task is still suspended, when a <see cref="SuspendTaskFor"/> times out
        /// (on the core's timer thread) or its task is cancelled (on the thread that calls
        /// <see cref="CancelTask"/>, or on a scheduler thread). Must be short, must not resume the
        /// task itself and must not cancel a task. The default, <c>false</c>, makes a wait neither time
        /// out nor end on a cancel.
        /// </remarks>
        virtual bool OnWithdraw([[maybe_unused]] SuspendedTask suspendedTask) noexcept
        {
            return false;
        }
    };

    /// <summary>Is told when a task has finished.</summary>
    struct CT_NOVTABLE INotifyTaskStateChange {
        /// <summary>Called by the core once the task has exited.</summary>
        /// <remarks>
        /// A task exits when its <see cref="ITask::Run"/> has returned, or when it exits without
        /// running: cancelled before it started (<see cref="CancelTask"/>), or added by
        /// <see cref="ITaskScheduler::AddTimedTask"/> while timers stop (<see cref="DisableTimers"/>).
        /// Called on a scheduler thread of the task's scheduler, from the fiber that ran the task;
        /// the core no longer touches the task object afterwards, so this is the earliest point at
        /// which the object may be destroyed or added again. Must not throw.
        /// </remarks>
        /// <seealso cref="ITask::SetNotifyCallback"/>
        virtual void OnTaskExited() noexcept = 0;
    };

    /// <summary>Storage the core keeps per task to control its start and its waits.</summary>
    /// <remarks>
    /// Lives inside a task object to start the task later
    /// (<see cref="ITaskScheduler::AddTimedTask"/>), and in every task context for
    /// <see cref="SleepTask"/> and <see cref="SuspendTaskFor"/>. Opaque; zero-initialized it means
    /// "idle".
    /// </remarks>
    /// <seealso cref="ITask::GetControlData"/>
    struct TaskControlData {
        void* reserved[16];
    };

    /// <summary>A unit of work the scheduler runs on one of its fibers.</summary>
    /// <remarks>
    /// The object is owned by the caller and must stay alive from
    /// <see cref="ITaskScheduler::AddTask"/> until its
    /// <see cref="INotifyTaskStateChange::OnTaskExited"/> has been called.
    /// </remarks>
    /// <seealso cref="Tasks::Task"/>
    struct CT_NOVTABLE ITask : INotifyTaskStateChange {
        /// <summary>The task's body.</summary>
        /// <remarks>
        /// Run once per <see cref="ITaskScheduler::AddTask"/> on a scheduler thread. It may suspend
        /// (wait on synchronization primitives), yield and sleep; the thread runs other tasks
        /// meanwhile.
        /// </remarks>
        virtual void Run() noexcept = 0;

        /// <summary>Sets the object that is told when the task exits.</summary>
        /// <param name="notifyTaskStateChange">
        /// The observer, borrowed: it must stay valid until the task has exited. <c>nullptr</c>: none.
        /// </param>
        /// <remarks>
        /// The core never calls it; it lets code that did not create the task observe its end (see
        /// <see cref="Tasks::Task"/>).
        /// </remarks>
        /// <seealso cref="INotifyTaskStateChange::OnTaskExited"/>
        virtual void SetNotifyCallback(INotifyTaskStateChange* notifyTaskStateChange) noexcept = 0;

        /// <summary>Returns the task's control data.</summary>
        /// <returns>
        /// Storage owned by the task object and valid as long as it lives, or <c>nullptr</c> (the
        /// default) if the task brings none and so cannot be started by time.
        /// </returns>
        /// <remarks>
        /// Needed only to start the task by time (<see cref="ITaskScheduler::AddTimedTask"/>): a task
        /// that has not started has no task context yet. A task that returns <c>nullptr</c> - one
        /// without state, say - cannot be started by time; it can still sleep and wait with a
        /// timeout.
        /// </remarks>
        /// <seealso cref="TaskControlData"/>
        virtual TaskControlData* GetControlData() noexcept { return nullptr; }
    };

    /// <summary>Decides when a <see cref="SchedulerKind::TaskSwap"/> scheduler switches tasks.</summary>
    /// <seealso cref="SchedulerInfo::taskSwitchPolicy"/>
    struct CT_NOVTABLE ITaskSwitchPolicy {
        /// <summary>Returns whether the scheduler should switch away from the running task now.</summary>
        /// <param name="currentTask">The task the scheduler is running at the moment; borrowed.</param>
        /// <returns><c>true</c> to switch to the other task; <c>false</c> to keep the current one.</returns>
        /// <remarks>Called on the scheduler's thread; must be short and must not suspend.</remarks>
        virtual bool ShouldSwitch(ITask* currentTask) noexcept = 0;
    };

    /// <summary>A scheduler: a set of OS threads that run tasks as fibers.</summary>
    /// <remarks>
    /// Created by <see cref="CreateTaskScheduler"/>, released by <see cref="Destroy"/>.
    /// User code needs <see cref="AddTask"/>, <see cref="AddTimedTask"/>, <see cref="Start"/>,
    /// <see cref="Stop"/> and <see cref="Destroy"/>. <see cref="SuspendTask"/>,
    /// <see cref="YieldTask"/>, <see cref="ResumeTask"/> and <see cref="StealTaskOrSuspend"/> are
    /// core-internal; use the free functions of the same name, which find the right scheduler.
    /// </remarks>
    /// <seealso cref="GetTaskScheduler"/>
    struct CT_NOVTABLE ITaskScheduler {
        /// <summary>Queues a task to run on this scheduler.</summary>
        /// <param name="task">
        /// The task to run; required. Owned by the caller and must stay valid until its
        /// <see cref="INotifyTaskStateChange::OnTaskExited"/> has been called.
        /// </param>
        /// <param name="priority">
        /// <see cref="TaskPriority::Default"/> inherits the calling task's priority.
        /// </param>
        /// <remarks>
        /// May be called from any thread or task, also before <see cref="Start"/> (the task then runs
        /// once Start has been called). Never blocks; the queue capacity must cover the runnable
        /// tasks (see <see cref="SchedulerInfo::taskQueueCapacity"/>).
        /// A task may be added again only once its <see cref="INotifyTaskStateChange::OnTaskExited"/>
        /// has been called.
        /// </remarks>
        /// <seealso cref="AddTimedTask"/>
        virtual void AddTask(ITask* task, TaskPriority priority = TaskPriority::Default) noexcept = 0;

        /// <summary>Starts the scheduler threads.</summary>
        /// <remarks>
        /// Call once. <see cref="SchedulerInfo::threadCallback"/> runs on each thread with
        /// <see cref="ThreadCallbackReason::Starting"/>.
        /// </remarks>
        /// <seealso cref="Stop"/>
        virtual void Start() noexcept = 0;

        /// <summary>Stops the scheduler and blocks until all its threads have exited.</summary>
        /// <remarks>
        /// Does nothing if the scheduler is not running. Tasks still queued or suspended are not run
        /// to completion, so wait for your tasks before stopping. Must be called from outside the
        /// scheduler's own threads.
        /// </remarks>
        /// <seealso cref="Start"/>
        virtual void Stop() noexcept = 0;

        /// <summary>Suspends the calling task.</summary>
        /// <param name="callback">
        /// Receives the task's handle once the task is off the processor; required, and must stay valid
        /// until <see cref="ISuspendCallback::OnSuspended"/> has returned.
        /// </param>
        /// <remarks>
        /// Core-internal: user code calls the free function
        /// <see cref="ThinkMeta::ConcurrentTasks::SuspendTask"/>, which forwards to the calling task's
        /// own scheduler. Called directly, it must be the scheduler running the calling task.
        /// </remarks>
        virtual void SuspendTask(ISuspendCallback* callback) noexcept = 0;

        /// <summary>Gives up the processor without suspending.</summary>
        /// <param name="yieldFlags">How to yield; see <see cref="YieldFlags"/>.</param>
        /// <remarks>
        /// Core-internal: user code calls the free function
        /// <see cref="ThinkMeta::ConcurrentTasks::YieldTask"/>. Called directly, it must be the
        /// scheduler running the calling task; the task stays runnable (see <see cref="YieldFlags"/>).
        /// </remarks>
        virtual void YieldTask(YieldFlags yieldFlags = YieldFlags::Default) noexcept = 0;

        /// <summary>Makes a suspended task runnable again.</summary>
        /// <param name="suspendedTask">
        /// Handle received by <see cref="ISuspendCallback::OnSuspended"/>; <c>nullptr</c> is ignored.
        /// </param>
        /// <remarks>
        /// Core-internal: user code calls the free function
        /// <see cref="ThinkMeta::ConcurrentTasks::ResumeTask"/>, which finds the right scheduler and
        /// also settles the race with a <see cref="SuspendTaskFor"/> timeout.
        /// Called directly, <paramref name="suspendedTask"/> must have been suspended on this
        /// scheduler; a handle from another scheduler must never be passed. Callable from any thread
        /// or task; <c>nullptr</c> is ignored.
        /// </remarks>
        virtual void ResumeTask(SuspendedTask suspendedTask) noexcept = 0;

        /// <summary>Fetches the next new task for an idle task fiber; internal to the core.</summary>
        /// <param name="taskPriority">Receives the returned task's priority.</param>
        /// <returns>
        /// The task, or <c>nullptr</c> after the fiber was switched away (suspended) because a
        /// resumed task or nothing is runnable.
        /// </returns>
        /// <remarks>Core-internal: never called by user code.</remarks>
        virtual ITask* StealTaskOrSuspend(TaskPriority& taskPriority) noexcept = 0;

        /// <summary>Releases a scheduler created by <see cref="CreateTaskScheduler"/>.</summary>
        /// <remarks>Use it instead of <c>delete</c>.</remarks>
        // The core may use a different allocator than the caller, so the object is freed inside
        // the core.
        virtual void Destroy() noexcept = 0;

        /// <summary>Adds a task after a delay.</summary>
        /// <param name="task">
        /// The task to start; it must return storage from <see cref="ITask::GetControlData"/>. Owned by the
        /// caller and must stay valid until its <see cref="INotifyTaskStateChange::OnTaskExited"/> has
        /// been called.
        /// </param>
        /// <param name="priority">
        /// Priority the task runs with; <see cref="TaskPriority::Default"/> inherits the calling task's
        /// priority.
        /// </param>
        /// <param name="delay">Time from now until the task is queued, as a <see cref="Duration"/>.</param>
        /// <returns>
        /// <c>true</c> if the task is waiting to start; <c>false</c> if timers are not enabled, the task has no
        /// <see cref="ITask::GetControlData"/>, it is being cancelled, or the scheduler cannot start
        /// tasks by time (<see cref="SchedulerKind::TaskSwap"/>).
        /// </returns>
        /// <remarks>
        /// Needs <see cref="EnableTimers"/>. Called again before the task has started, it moves the
        /// start. Like <see cref="AddTask"/>, a task may be added again only once it has exited.
        /// </remarks>
        /// <seealso cref="CancelTask"/>
        /// <seealso cref="ScheduleTimedTask"/>
        virtual bool AddTimedTask(ITask* task, TaskPriority priority, Duration delay) noexcept = 0;
    };

    /// <summary>A scheduler that runs its tasks on a thread with an event loop of its own, such as a UI thread.</summary>
    /// <remarks>
    /// Created by <see cref="CreateDispatcherScheduler"/> on that thread, which it is bound to. The
    /// scheduler has no threads; the event loop drives it. A task starts with <see cref="RunTask"/>,
    /// typically from an event handler, and runs at once until it ends or waits. When a waiting task
    /// can go on (<see cref="ResumeTask"/>, <see cref="AddTask"/>, a timer), the scheduler hands it to
    /// the loop through the <see cref="DispatcherWakeProc"/>, and the loop passes it to
    /// <see cref="ContinueTask"/>.
    ///
    /// The bound thread stays an ordinary thread: only the tasks can wait. A task always returns to
    /// whoever entered it last, so handlers that start or continue tasks get control back before they
    /// return themselves; tasks may also be started or continued from within other tasks and from
    /// modal loops inside them.
    ///
    /// <see cref="Start"/> does nothing. <see cref="Stop"/> and <see cref="Destroy"/> are called once
    /// no task waits or is on its way to the loop; Stop on the bound thread.
    /// </remarks>
    /// <seealso cref="CreateDispatcherScheduler"/>
    struct CT_NOVTABLE IDispatcherScheduler : ITaskScheduler {
        /// <summary>Runs a task now, on the bound thread.</summary>
        /// <param name="task">
        /// The task to run; required. Owned by the caller and must stay valid until its
        /// <see cref="INotifyTaskStateChange::OnTaskExited"/> has been called.
        /// </param>
        /// <remarks>
        /// Returns when the task has ended or waits. Only on the bound thread.
        /// </remarks>
        virtual void RunTask(ITask* task) noexcept = 0;

        /// <summary>Runs a task the scheduler handed to the event loop, on the bound thread.</summary>
        /// <param name="task">
        /// A handle the <see cref="DispatcherWakeProc"/> of this scheduler received; each exactly once.
        /// </param>
        /// <remarks>
        /// Continues a task that was resumed or yielded, or starts one added by
        /// <see cref="AddTask"/>. Returns when the task has ended or waits again. Only on the bound
        /// thread.
        /// </remarks>
        virtual void ContinueTask(SuspendedTask task) noexcept = 0;
    };

    /// <summary>Hands a task that can run to the event loop of a dispatcher scheduler.</summary>
    /// <param name="scheduler">The scheduler the task belongs to.</param>
    /// <param name="task">The task; the loop passes it to <see cref="IDispatcherScheduler::ContinueTask"/>.</param>
    /// <param name="context">The context given to <see cref="CreateDispatcherScheduler"/>.</param>
    /// <remarks>
    /// Called once for every task that can run, on any thread, also on the bound thread itself. It
    /// must not block and must not lose the task: a window posts a message carrying it, for example.
    /// A message to a window also reaches modal loops (message boxes, moving a window, menus), unlike a
    /// thread message. Posted messages are limited (10,000 per thread by default); every waiting task
    /// is on its way at most once.
    /// </remarks>
    /// <seealso cref="CreateDispatcherScheduler"/>
    using DispatcherWakeProc = void (*)(IDispatcherScheduler* scheduler, SuspendedTask task, void* context) noexcept;

    /// <summary>Called on each scheduler thread when it starts and when it exits.</summary>
    /// <param name="taskScheduler">The scheduler the thread belongs to.</param>
    /// <param name="threadIndex">0-based index of the thread within its scheduler.</param>
    /// <param name="thread">
    /// The scheduler thread's native handle: on Windows a <c>HANDLE</c> with full access, owned by
    /// the scheduler and valid until it is destroyed (duplicate it to keep it longer); elsewhere
    /// the <c>pthread_t</c>.
    /// </param>
    /// <param name="context"><see cref="SchedulerInfo::threadCallbackContext"/>.</param>
    /// <param name="reason">See <see cref="ThreadCallbackReason"/>.</param>
    /// <remarks>
    /// Use it e.g. to set thread affinity, priority or thread-local state.
    /// Runs on the plain OS thread, not in a task: it must not use the synchronization primitives
    /// or the task functions. Only the <see cref="SchedulerKind::Default"/> scheduler calls it.
    /// </remarks>
    /// <seealso cref="SchedulerInfo::threadCallback"/>
    using ThreadCallbackProc = void (*)(ITaskScheduler* taskScheduler, std::uint32_t threadIndex, NativeThread thread, void* context, ThreadCallbackReason reason) noexcept;

    /// <summary>Parameters of <see cref="CreateTaskScheduler"/>.</summary>
    /// <remarks>The core copies the struct.</remarks>
    struct SchedulerInfo {
        /// <summary>ABI version the caller was built with.</summary>
        /// <remarks>
        /// Leave it at <see cref="SchedulerVersion"/>. <see cref="CreateTaskScheduler"/> rejects any
        /// other version.
        /// </remarks>
        std::uint32_t version = SchedulerVersion;

        /// <summary>Scheduler threads to run; <c>0</c> means one per logical processor.</summary>
        std::uint32_t numberOfThreads = 0;

        /// <summary>Called on every scheduler thread when it starts and when it exits.</summary>
        /// <seealso cref="ThreadCallbackProc"/>
        ThreadCallbackProc threadCallback = nullptr;

        /// <summary>Passed unchanged to <see cref="threadCallback"/>.</summary>
        /// <remarks>Must outlive the scheduler's threads.</remarks>
        void* threadCallbackContext = nullptr;

        /// <summary>Switch policy of a <see cref="SchedulerKind::TaskSwap"/> scheduler.</summary>
        /// <remarks>Required for TaskSwap, ignored otherwise.</remarks>
        /// <seealso cref="ITaskSwitchPolicy"/>
        ITaskSwitchPolicy* taskSwitchPolicy = nullptr;

        /// <summary>Capacity of each of the Default scheduler's four task queues.</summary>
        /// <remarks>
        /// It bounds the tasks that can be runnable at the same time per queue - new tasks per
        /// priority, and resumed or yielded tasks per priority - so choose it larger than the most
        /// tasks the application can have runnable at once; filling a queue is invalid use.
        /// Suspended tasks do not count. Rounded up to the next power of two; <c>0</c> means
        /// <see cref="DefaultTaskQueueCapacity"/>; above <see cref="MaximumTaskQueueCapacity"/>
        /// <see cref="CreateTaskScheduler"/> fails. Each slot takes one cache line. Ignored by
        /// <see cref="SchedulerKind::TaskSwap"/>.
        /// </remarks>
        std::uint32_t taskQueueCapacity = 0;

        /// <summary>Size of the stack of each task, in bytes.</summary>
        /// <remarks>
        /// Address space reserved per task; memory is committed as the stack grows. <c>0</c> means
        /// the platform's default: on Windows the stack reserve of the executable's header (1 MiB
        /// unless the linker was told otherwise), on Linux 1 MiB. Rounded up to the granularity of
        /// address space (64 KiB on Windows, the page size on Linux). Every task context the
        /// scheduler creates gets a stack of this size; a task's stack must hold its deepest call
        /// chain, as there is no way to grow it later. A <see cref="SchedulerKind::Default"/> and a
        /// <see cref="SchedulerKind::TaskSwap"/> scheduler honour it; a dispatcher scheduler uses
        /// the default. Below each stack lies an inaccessible guard zone (Linux: 64 KiB, as clang
        /// probes large frames only with <c>-fstack-clash-protection</c>), so an overflow faults.
        /// On Linux a signal handler installed without <c>SA_ONSTACK</c> runs on the stack of the
        /// task it interrupts and needs room there; the core installs no alternate signal stack.
        /// Linux shadow stacks (CET, <c>-fcf-protection</c>) are not supported: the fiber switch
        /// carries no shadow-stack note, so glibc turns them off for the process or refuses it.
        /// </remarks>
        std::uint32_t taskStackSize = 0;
    };

    extern "C" {
    /// <summary>Creates a scheduler.</summary>
    /// <param name="kind">The scheduler implementation; see <see cref="SchedulerKind"/>.</param>
    /// <param name="info">
    /// The scheduler's parameters; required. The core copies the struct, so it need only be valid
    /// during the call.
    /// </param>
    /// <returns>
    /// The scheduler, or <c>nullptr</c> if <paramref name="info"/> is null, its version is
    /// unknown, <paramref name="kind"/> needs a field that is not set,
    /// <see cref="SchedulerInfo::taskQueueCapacity"/> is too large, or the queues cannot be
    /// allocated.
    /// </returns>
    /// <seealso cref="ITaskScheduler::Destroy"/>
    CT_CORE ITaskScheduler* CreateTaskScheduler(SchedulerKind kind, const SchedulerInfo* info) noexcept;

    /// <summary>Creates a dispatcher scheduler bound to the calling thread.</summary>
    /// <param name="wake">Hands tasks that can run to the thread's event loop; required.</param>
    /// <param name="context">Passed unchanged to <paramref name="wake"/>; must outlive the scheduler.</param>
    /// <returns>The scheduler, or <c>nullptr</c> if <paramref name="wake"/> is null or memory ran out.</returns>
    /// <remarks>
    /// The calling thread is bound, not converted: outside the scheduler's tasks it stays an ordinary
    /// thread, and <see cref="GetTaskScheduler"/> works only inside them.
    /// </remarks>
    /// <seealso cref="IDispatcherScheduler"/>
    /// <seealso cref="ITaskScheduler::Destroy"/>
    CT_CORE IDispatcherScheduler* CreateDispatcherScheduler(DispatcherWakeProc wake, void* context) noexcept;

    /// <summary>Returns the scheduler running the calling task.</summary>
    /// <returns>The scheduler; owned by its creator, not by the caller.</returns>
    /// <remarks>Only in a task or a converted thread (<see cref="ConvertThreadToTask"/>).</remarks>
    CT_CORE ITaskScheduler* GetTaskScheduler() noexcept;

    /// <summary>Returns the handle of the calling task.</summary>
    /// <returns>The calling task's handle, valid as long as the task (or converted thread) exists.</returns>
    /// <remarks>
    /// Only in a task or a converted thread (<see cref="ConvertThreadToTask"/>); on any other
    /// thread the result is undefined and the call may crash.
    /// </remarks>
    /// <seealso cref="GetTask"/>
    CT_CORE TaskHandle GetCurrentTask() noexcept;

    /// <summary>Returns the task object behind a handle.</summary>
    /// <param name="task">A task's handle (a <see cref="SuspendedTask"/> converts to it); <c>nullptr</c> stands for the calling task.</param>
    /// <returns>The <see cref="ITask"/> the handle runs; owned by whoever added the task.</returns>
    /// <seealso cref="GetCurrentTask"/>
    CT_CORE ITask* GetTask(TaskHandle task) noexcept;

    /// <summary>Suspends the calling task.</summary>
    /// <param name="callback">
    /// Receives the task's handle once the task is off the processor; required, and must stay valid
    /// until <see cref="ISuspendCallback::OnSuspended"/> has returned.
    /// </param>
    /// <remarks>
    /// The core calls back into <see cref="ISuspendCallback::OnSuspended"/> with the task's
    /// handle once the task is off the processor, so the waiter can publish it.
    /// </remarks>
    /// <seealso cref="ResumeTask"/>
    /// <seealso cref="SuspendTaskFor"/>
    CT_CORE void SuspendTask(ISuspendCallback* callback) noexcept;

    /// <summary>Makes a suspended task runnable again.</summary>
    /// <param name="suspendedTask">
    /// Handle received by <see cref="ISuspendCallback::OnSuspended"/>; <c>nullptr</c> is ignored.
    /// </param>
    /// <remarks>
    /// The task runs on the scheduler that suspended it. Callable from any task or OS thread, also
    /// from a completion routine on the core's I/O thread; never blocks. Resume each suspension
    /// exactly once.
    /// </remarks>
    /// <seealso cref="SuspendTask"/>
    CT_CORE void ResumeTask(SuspendedTask suspendedTask) noexcept;

    /// <summary>Gives up the processor without suspending the calling task.</summary>
    /// <param name="yieldFlags">How to yield; see <see cref="YieldFlags"/>.</param>
    /// <seealso cref="YieldFlags"/>
    CT_CORE void YieldTask(YieldFlags yieldFlags = YieldFlags::Default) noexcept;

    /// <summary>Sets the priority of the calling task.</summary>
    /// <param name="priority">
    /// <see cref="TaskPriority::Normal"/> or <see cref="TaskPriority::High"/>;
    /// <see cref="TaskPriority::Default"/> is not meaningful here.
    /// </param>
    /// <seealso cref="TaskPriority"/>
    CT_CORE void SetTaskPriority(TaskPriority priority) noexcept;

    /// <summary>Lets the calling OS thread act as a task.</summary>
    /// <remarks>
    /// The thread can then use the synchronization primitives.
    /// <see cref="ConvertTaskToThread"/> reverses it. Once per thread: a second call before
    /// ConvertTaskToThread is invalid use (the thread's task state would be nested, and the
    /// first one lost).
    /// </remarks>
    CT_CORE void ConvertThreadToTask() noexcept;

    /// <summary>Reverses <see cref="ConvertThreadToTask"/> on the calling thread.</summary>
    /// <remarks>
    /// Releases the thread's task state. Call it on the same thread, once no other task can still
    /// resume this thread's task.
    /// </remarks>
    CT_CORE void ConvertTaskToThread() noexcept;

    // --- cancellation ---

    /// <summary>Asks a task to cancel.</summary>
    /// <param name="task">The task; it must return storage from <see cref="ITask::GetControlData"/>.</param>
    /// <returns>
    /// <c>true</c> if the request is new; <c>false</c> if the task was asked already or has no
    /// <see cref="ITask::GetControlData"/>.
    /// </returns>
    /// <remarks>
    /// Cooperative: a running task is never stopped from outside; it sees the request through
    /// <see cref="IsCancellationRequested"/> and ends itself. A task that has not started yet - added,
    /// or waiting to start by time - exits without running
    /// (<see cref="INotifyTaskStateChange::OnTaskExited"/>). A <see cref="SleepTask"/> ends early, and
    /// a <see cref="SuspendTaskFor"/> ends with <see cref="WaitStatus::Cancelled"/> if its
    /// <see cref="ISuspendCallback::OnWithdraw"/> takes the task back; a later one returns at once.
    /// The request stays until the task has exited; asked while the task is not added at all, it
    /// applies to the next run, which then exits without running. Asked again for a standing
    /// request, it acts again, e.g. on a start by time added since, and returns <c>false</c>.
    /// Callable from any task or thread, but not from <see cref="ISuspendCallback::OnWithdraw"/>;
    /// the task object must stay valid during the call. Never blocks.
    /// </remarks>
    /// <seealso cref="IsCancellationRequested"/>
    CT_CORE bool CancelTask(ITask* task) noexcept;

    /// <summary>Returns whether the calling task is asked to cancel.</summary>
    /// <returns>
    /// <c>true</c> after a <see cref="CancelTask"/> for the calling task; <c>false</c> otherwise, also
    /// outside a task and in a task without <see cref="ITask::GetControlData"/>.
    /// </returns>
    /// <remarks>Cheap enough to ask in a loop.</remarks>
    /// <seealso cref="CancelTask"/>
    CT_CORE bool IsCancellationRequested() noexcept;

    // --- tasks by time ---

    /// <summary>Starts the core's timer thread.</summary>
    /// <returns><c>true</c> if timers are enabled; <c>false</c> only if the thread could not be started.</returns>
    /// <remarks>
    /// The timer thread starts tasks added by <see cref="ITaskScheduler::AddTimedTask"/>, wakes
    /// sleeping tasks and ends waits that time out. Calls are counted like
    /// <see cref="AsyncIo::EnableAsyncIo"/>.
    /// </remarks>
    /// <seealso cref="DisableTimers"/>
    CT_CORE bool EnableTimers() noexcept;

    /// <summary>Takes back one <see cref="EnableTimers"/>.</summary>
    /// <returns><c>true</c> on success; <c>false</c> if timers were not enabled.</returns>
    /// <remarks>
    /// The last one ends what is still waiting: tasks not yet started exit without running
    /// (<see cref="INotifyTaskStateChange::OnTaskExited"/>, on their scheduler, which must still
    /// run), sleeping tasks wake, and waits with a timeout time out. No other thread may use
    /// timers meanwhile.
    /// </remarks>
    CT_CORE bool DisableTimers() noexcept;

    /// <summary>Suspends the calling task for a while.</summary>
    /// <param name="duration">Minimum time to sleep, as a <see cref="Duration"/>.</param>
    /// <returns>
    /// <c>true</c> after the sleep; <c>false</c> at once if timers are not enabled, and early if the
    /// task is cancelled (<see cref="CancelTask"/>).
    /// </returns>
    /// <remarks>
    /// Its thread runs other tasks meanwhile. Only a cancel cuts the sleep short; to wait for
    /// something with a time limit, wait on it with a timeout.
    /// </remarks>
    /// <seealso cref="EnableTimers"/>
    CT_CORE bool SleepTask(Duration duration) noexcept;

    /// <summary>Suspends the calling task with a timeout; the wait also ends when the task is cancelled.</summary>
    /// <param name="callback">
    /// Receives the task's handle (<see cref="ISuspendCallback::OnSuspended"/>) and, on timeout or
    /// cancel, the <see cref="ISuspendCallback::OnWithdraw"/> call; required, and must stay valid
    /// until this call returns.
    /// </param>
    /// <param name="timeout">
    /// Longest time to stay suspended; <see cref="Duration::Infinite"/> waits without a timeout.
    /// </param>
    /// <returns>
    /// <see cref="WaitStatus::Signaled"/> if a <see cref="ResumeTask"/> ended the wait;
    /// <see cref="WaitStatus::TimedOut"/> or <see cref="WaitStatus::Cancelled"/> if the core ended
    /// it: then <see cref="ISuspendCallback::OnWithdraw"/> on <paramref name="callback"/> took the
    /// task back from where <see cref="ISuspendCallback::OnSuspended"/> had published it.
    /// <see cref="WaitStatus::Cancelled"/> at once, without suspending, if the task was asked to
    /// cancel before.
    /// </returns>
    /// <remarks>
    /// Like <see cref="SuspendTask"/>, but the wait ends after <paramref name="timeout"/> at the
    /// latest, or on <see cref="CancelTask"/> for a task with <see cref="ITask::GetControlData"/>.
    /// The waiter's state (the callback) is never touched again once the task runs.
    /// Without <see cref="EnableTimers"/>, or with <see cref="Duration::Infinite"/>, it waits
    /// without a timeout.
    /// </remarks>
    CT_CORE WaitStatus SuspendTaskFor(ISuspendCallback* callback, Duration timeout) noexcept;

    /// <summary>Starts a timed task now instead of later.</summary>
    /// <param name="task">A task added by <see cref="ITaskScheduler::AddTimedTask"/>.</param>
    /// <returns><c>true</c> if the task was queued now; <c>false</c> if it was not waiting to start.</returns>
    /// <remarks>Never blocks. To take the start back, cancel the task (<see cref="CancelTask"/>).</remarks>
    /// <seealso cref="CancelTask"/>
    CT_CORE bool ScheduleTimedTask(ITask* task) noexcept;
    }

    // --- asynchronous I/O: the platform's own part of the ABI ---
    //
    // Windows: the core owns an I/O completion port and the thread draining it; the caller opens
    // handles for overlapped I/O, associates them and starts the operations itself. Linux: the
    // core owns an io_uring and the thread draining it; the caller describes each operation in an
    // IoRequest and the core submits it, since io_uring has the owner of the ring submit.

    namespace AsyncIo {
        extern "C" {

        /// <summary>Starts the core's asynchronous I/O and the thread that completes it.</summary>
        /// <returns><c>true</c> if asynchronous I/O is enabled; <c>false</c> only if it could not be started.</returns>
        /// <remarks>
        /// Windows: an I/O completion port. Linux: an io_uring, which some environments block (the
        /// default seccomp profile of Docker, kernels built without it); there it returns
        /// <c>false</c>, and the schedulers and primitives work as before.
        /// Calls are counted: only the first one starts it, so a library and the application can
        /// both enable asynchronous I/O.
        /// </remarks>
        /// <seealso cref="DisableAsyncIo"/>
        CT_CORE bool EnableAsyncIo() noexcept;

        /// <summary>Takes back one <see cref="EnableAsyncIo"/>.</summary>
        /// <returns><c>true</c> on success; <c>false</c> if asynchronous I/O was not enabled.</returns>
        /// <remarks>
        /// The last one stops the asynchronous I/O and its thread, and by then no operation may be
        /// pending.
        /// </remarks>
        CT_CORE bool DisableAsyncIo() noexcept;

#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>Called for every completion packet of an associated handle.</summary>
        /// <param name="overlapped">
        /// The <c>OVERLAPPED</c> the operation was started with; the caller usually embeds it in its own
        /// request object to find that again.
        /// </param>
        /// <param name="error">
        /// The operation's own code (<c>ERROR_SUCCESS</c> when it succeeded), not a thread-local
        /// last error.
        /// </param>
        /// <param name="bytesTransferred">Number of bytes the operation transferred.</param>
        /// <remarks>
        /// Runs on the core's I/O thread for handles associated through
        /// <see cref="AssociateHandleWithIoCompletionPort"/>.
        /// </remarks>
        using IoCompletionRoutine = void (*)(OVERLAPPED* overlapped, DWORD error, DWORD bytesTransferred) noexcept;

        /// <summary>Associates a file or socket handle with the core's completion port.</summary>
        /// <param name="handle">
        /// A file or socket handle opened for overlapped I/O (<c>FILE_FLAG_OVERLAPPED</c>); borrowed, it
        /// stays associated until it is closed.
        /// </param>
        /// <param name="completionRoutine">Called for every completion packet of <paramref name="handle"/>; required.</param>
        /// <returns>
        /// <c>true</c> on success; <c>false</c> if it failed (<c>GetLastError</c> has the reason; <c>ERROR_NOT_READY</c> when
        /// <see cref="EnableAsyncIo"/> has not been called).
        /// </returns>
        /// <remarks>
        /// The routine is stored as the handle's completion key, so the core needs no knowledge of
        /// the caller's types.
        /// </remarks>
        /// <seealso cref="IoCompletionRoutine"/>
        /// <seealso cref="EnableAsyncIo"/>
        CT_CORE bool AssociateHandleWithIoCompletionPort(HANDLE handle, IoCompletionRoutine completionRoutine) noexcept;
#else
        /// <summary>The operation an <see cref="IoRequest"/> describes.</summary>
        enum class IoOperation : std::uint8_t {
            /// <summary><c>pread</c>: <c>length</c> bytes from the file at <c>offset</c> into <c>buffer</c>.</summary>
            Read,
            /// <summary><c>pwrite</c>: <c>length</c> bytes from <c>buffer</c> to the file at <c>offset</c>.</summary>
            Write,
            /// <summary><c>recv</c> into <c>buffer</c>, at most <c>length</c> bytes, with the <c>MSG_*</c> <c>flags</c>.</summary>
            Recv,
            /// <summary><c>send</c> of <c>length</c> bytes from <c>buffer</c>, with the <c>MSG_*</c> <c>flags</c>.</summary>
            Send,
            /// <summary><c>recvmsg</c>: <c>buffer</c> is the <c>msghdr</c>, <c>flags</c> the <c>MSG_*</c> flags.</summary>
            RecvMsg,
            /// <summary><c>sendmsg</c>: <c>buffer</c> is the <c>msghdr</c>, <c>flags</c> the <c>MSG_*</c> flags.</summary>
            SendMsg,
            /// <summary>
            /// <c>accept4</c>: <c>buffer</c> receives the peer's address (or is <c>nullptr</c>),
            /// <c>offset</c> is the address of the <c>socklen_t</c> with its size, <c>flags</c> the
            /// <c>SOCK_*</c> flags of the new socket. The result is the new socket.
            /// </summary>
            Accept,
            /// <summary><c>connect</c> to the address in <c>buffer</c>, which is <c>length</c> bytes long.</summary>
            Connect,
        };

        struct IoRequest;

        /// <summary>Called once for every request <see cref="SubmitIo"/> accepted, when it has completed.</summary>
        /// <param name="request">The request as it was submitted.</param>
        /// <param name="result">
        /// What the operation returned: the bytes transferred (or the new socket of
        /// <see cref="IoOperation::Accept"/>) when it succeeded, else the negated <c>errno</c>
        /// (<c>-ECANCELED</c> once <see cref="CancelIo"/> or <see cref="CancelAllIo"/> ended it; an
        /// operation the kernel was already carrying out may report <c>-EINTR</c> instead).
        /// </param>
        /// <remarks>Runs on the core's I/O thread.</remarks>
        using IoCompletionRoutine = void (*)(IoRequest* request, std::int32_t result) noexcept;

        /// <summary>One asynchronous operation, described by the caller and carried out by the core.</summary>
        /// <remarks>
        /// Usually the base of the caller's own request object, which the routine casts back to.
        /// The request, its buffer and whatever <c>offset</c> points to belong to the operation from
        /// <see cref="SubmitIo"/> until the routine has been called.
        /// </remarks>
        struct IoRequest {
            /// <summary>Called when the operation has completed; required.</summary>
            IoCompletionRoutine routine;
            /// <summary>The data, or the structure the operation names (see <see cref="IoOperation"/>).</summary>
            void* buffer;
            /// <summary>The file offset of Read and Write; see <see cref="IoOperation"/> for the others; else 0.</summary>
            std::uint64_t offset;
            /// <summary>The file or socket.</summary>
            std::int32_t fd;
            /// <summary>The number of bytes, or the size of what <c>buffer</c> points to.</summary>
            std::uint32_t length;
            /// <summary>The flags of the operation (see <see cref="IoOperation"/>); else 0.</summary>
            std::uint32_t flags;
            /// <summary>The operation.</summary>
            IoOperation operation;
            /// <summary>Belongs to the core; 0 before the request is submitted.</summary>
            std::uint8_t reserved8[3];
            /// <summary>Belongs to the core; 0 before the request is submitted.</summary>
            std::uint64_t reserved[2];
        };

        /// <summary>Starts the operation a request describes.</summary>
        /// <param name="request">The request; required. It belongs to the operation until its routine has been called.</param>
        /// <returns>
        /// <c>true</c> if the operation was started: its routine will be called exactly once.
        /// <c>false</c> if not, with <c>errno</c> set: <c>ENODEV</c> when
        /// <see cref="EnableAsyncIo"/> has not been called, <c>EAGAIN</c> or <c>EBUSY</c> when the
        /// kernel takes nothing more for the moment (try again later); the routine is not called.
        /// </returns>
        /// <remarks>Callable from any thread.</remarks>
        /// <seealso cref="CancelIo"/>
        CT_CORE bool SubmitIo(IoRequest* request) noexcept;

        /// <summary>Asks the kernel to end a submitted operation early.</summary>
        /// <param name="request">A request <see cref="SubmitIo"/> accepted.</param>
        /// <returns>
        /// <c>true</c> if the request to cancel was passed on; <c>false</c> with <c>errno</c> set
        /// otherwise, as for <see cref="SubmitIo"/>.
        /// </returns>
        /// <remarks>
        /// Does not wait. The operation still completes through its routine, with <c>-ECANCELED</c>
        /// or, if it had already finished, with its own result; a request that has completed
        /// already is not affected.
        /// </remarks>
        CT_CORE bool CancelIo(IoRequest* request) noexcept;

        /// <summary>Asks the kernel to end every submitted operation on a file or socket early.</summary>
        /// <param name="fd">The file or socket; must still be open.</param>
        /// <returns>
        /// <c>true</c> if the request to cancel was passed on; <c>false</c> with <c>errno</c> set
        /// otherwise: as for <see cref="CancelIo"/>, <c>EINVAL</c> for a negative <paramref name="fd"/>,
        /// and <c>EOPNOTSUPP</c> when the kernel cannot cancel by file (before Linux 5.19).
        /// </returns>
        /// <remarks>
        /// The counterpart of <c>CancelIoEx</c> without an <c>OVERLAPPED</c>. Does not wait. Covers the
        /// operations submitted before the call; each still completes through its routine, with
        /// <c>-ECANCELED</c> or its own result. Unlike Windows, <c>true</c> does not tell whether any
        /// operation was pending. Call it before closing <paramref name="fd"/>: closing does not end
        /// the operations, and the number may then be given to another file.
        /// </remarks>
        /// <seealso cref="CancelIo"/>
        CT_CORE bool CancelAllIo(std::int32_t fd) noexcept;
#endif
        }
    } // namespace AsyncIo

    /// <summary>Returns the task object of the calling task.</summary>
    /// <returns>The calling task's <see cref="ITask"/>.</returns>
    /// <seealso cref="GetCurrentTask"/>
    inline ITask* GetTask() noexcept
    {
        return GetTask(nullptr);
    }
} // namespace ThinkMeta::ConcurrentTasks
