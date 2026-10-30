//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// DispatcherScheduler: owns a dispatcher scheduler of the core (CreateDispatcherScheduler) and,
// for a window, supplies the wake callback: every task that can go on is posted to the window as a
// message, and the window procedure hands it back with ContinueTask.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Schedulers.DispatcherScheduler;

namespace ThinkMeta::ConcurrentTasks::Schedulers {
    /// <summary>Owns a dispatcher scheduler bound to the calling thread.</summary>
    /// <remarks>
    /// Lets event handlers on a UI thread wait without blocking it: start a handler with
    /// <see cref="RunTask"/>; when it waits, RunTask returns, and when it can go on, the scheduler
    /// hands it to the thread's event loop, which passes it to <see cref="ContinueTask"/>. Work for
    /// other threads goes to another scheduler explicitly (<see cref="Tasks::Task::Schedule(ITaskScheduler&amp;, TaskPriority)"/>,
    /// <see cref="Tasks::RunAndWait"/>).
    ///
    /// Created and destroyed on the bound thread; destroyed only once no task waits or is on its way
    /// to the loop. Not copyable or movable: the scheduler keeps this object's address as the wake
    /// context.
    ///
    /// Usage rules: wait only inside the scheduler's tasks, never in other code on the bound thread;
    /// do not wait inside an activation context of your own (<c>ActivateActCtx</c> … wait …
    /// <c>DeactivateActCtx</c>).
    /// </remarks>
    /// <seealso cref="IDispatcherScheduler"/>
    export class DispatcherScheduler
    {
    public:
        /// <summary>Creates the scheduler with a wake callback of the caller's.</summary>
        /// <param name="wake">Hands tasks that can go on to the event loop; see <see cref="DispatcherWakeProc"/>.</param>
        /// <param name="context">Passed to <paramref name="wake"/>; must outlive the scheduler.</param>
        DispatcherScheduler(DispatcherWakeProc wake, void* context) noexcept : scheduler_(CreateDispatcherScheduler(wake, context)) {}

        /// <summary>Creates the scheduler for a window: tasks that can go on are posted to it.</summary>
        /// <param name="window">The window that receives the tasks; may be set later with <see cref="SetWindow"/>.</param>
        /// <param name="message">
        /// The message that carries a task in its <c>lParam</c>; the window procedure passes it to
        /// <see cref="ContinueTask(LPARAM)"/>. Typically <c>WM_APP + n</c>.
        /// </param>
        /// <remarks>
        /// A message to a window also reaches modal loops (message boxes, moving the window, menus),
        /// so waiting handlers go on there too.
        /// A task that cannot be posted - no window set yet, the window destroyed, the thread's
        /// message queue full (10,000 posted messages by default) - would wait for ever; the process
        /// ends instead (<c>__fastfail</c>), also in release builds.
        /// </remarks>
        DispatcherScheduler(HWND window, UINT message) noexcept
            : scheduler_(CreateDispatcherScheduler(&DispatcherScheduler::PostToWindow, this)), window_(window), message_(message)
        {
        }

        DispatcherScheduler(const DispatcherScheduler&)            = delete;
        DispatcherScheduler(DispatcherScheduler&&)                 = delete;
        DispatcherScheduler& operator=(const DispatcherScheduler&) = delete;
        DispatcherScheduler& operator=(DispatcherScheduler&&)      = delete;

        /// <summary>Stops and destroys the scheduler.</summary>
        ~DispatcherScheduler()
        {
            if (scheduler_) {
                scheduler_->Stop();
                scheduler_->Destroy();
            }
        }

        /// <summary>Returns whether the scheduler was created.</summary>
        explicit operator bool() const noexcept { return scheduler_ != nullptr; }

        /// <summary>Returns the core's scheduler, e.g. for <see cref="ITaskScheduler::AddTask"/> from another thread.</summary>
        IDispatcherScheduler* Get() const noexcept { return scheduler_; }

        /// <summary>Returns the core's scheduler.</summary>
        IDispatcherScheduler* operator->() const noexcept { return scheduler_; }

        /// <summary>Sets the window for the window constructor, if it did not exist yet then.</summary>
        /// <param name="window">The window; set it before the first task can wait.</param>
        void SetWindow(HWND window) noexcept { window_ = window; }

        /// <summary>Runs a task now; returns when it has ended or waits.</summary>
        /// <param name="task">The task; must stay valid until it has exited.</param>
        /// <remarks>Bound thread only, typically from an event handler.</remarks>
        /// <seealso cref="IDispatcherScheduler::RunTask"/>
        void RunTask(ITask* task) noexcept { scheduler_->RunTask(task); }

        /// <summary>Continues a task the event loop got; returns when it has ended or waits again.</summary>
        /// <param name="task">A task handed to the event loop by this scheduler; each exactly once.</param>
        /// <seealso cref="IDispatcherScheduler::ContinueTask"/>
        void ContinueTask(SuspendedTask task) noexcept { scheduler_->ContinueTask(task); }

        /// <summary>Continues the task carried by a message of the window constructor.</summary>
        /// <param name="lParam">The <c>lParam</c> of the message.</param>
        void ContinueTask(LPARAM lParam) noexcept { scheduler_->ContinueTask(reinterpret_cast<SuspendedTask>(lParam)); }

    private:
        static void PostToWindow(IDispatcherScheduler*, SuspendedTask task, void* context) noexcept
        {
            auto* self = static_cast<DispatcherScheduler*>(context);
            // A lost message would leave the task waiting for ever, a hang hard to trace back; end
            // the process here instead. Without a window, PostMessage would post a thread message
            // to the calling thread - often a worker - and report success.
            CT_ASSERT(self->window_ != nullptr);
            if (self->window_ == nullptr || !PostMessageW(self->window_, self->message_, 0, reinterpret_cast<LPARAM>(task)))
                __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        }

        IDispatcherScheduler* scheduler_;
        HWND                  window_  = nullptr;
        UINT                  message_ = 0;
    };
} // namespace ThinkMeta::ConcurrentTasks::Schedulers
