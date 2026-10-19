//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// ConvenienceThread: base class for an OS thread whose body is a virtual Run, on top of
// Windows::Thread.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <cstddef>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.ConvenienceThread;

import ThinkMeta.ConcurrentTasks.Windows.Thread;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Base class for a plain OS thread whose body is <see cref="Run"/>.</summary>
    /// <remarks>
    /// Derive, implement Run, call <see cref="Create"/>. Run executes on the new thread, which is
    /// not a task (call <see cref="ConvertThreadToTask"/> in Run to use the task primitives).
    /// The object must outlive the thread: wait for it (<see cref="Wait"/>) before destroying the
    /// object, because Run is called on it. The destructor closes the handle but does not wait.
    /// </remarks>
    /// <seealso cref="Thread"/>
    export class CT_NOVTABLE ConvenienceThread
    {
        ConvenienceThread(const ConvenienceThread&)            = delete;
        ConvenienceThread(ConvenienceThread&&)                 = delete;
        ConvenienceThread& operator=(const ConvenienceThread&) = delete;
        ConvenienceThread& operator=(ConvenienceThread&&)      = delete;

    public:
        /// <summary>Starts the thread, which calls <see cref="Run"/>.</summary>
        /// <param name="stackSize">Initial stack size in bytes; <c>0</c> means the default stack size.</param>
        /// <returns><see cref="Error::Success"/> if the thread was started; otherwise the Win32 error.</returns>
        /// <remarks>Call once.</remarks>
        /// <seealso cref="Wait"/>
        ErrorCode Create(std::size_t stackSize = 0) noexcept { return thread_.Create(nullptr, stackSize, ThreadProc, this, 0, nullptr); }

        /// <summary>Returns whether the thread handle is open (created and not closed).</summary>
        /// <returns><c>true</c> if the handle is open; otherwise <c>false</c>.</returns>
        bool IsValid() const noexcept { return thread_.IsValid(); }

        /// <summary>Closes the thread handle.</summary>
        /// <returns><see cref="Error::Success"/>, also if no handle was open; otherwise the Win32 error.</returns>
        /// <remarks>The thread keeps running; <see cref="Wait"/> is no longer possible afterwards.</remarks>
        ErrorCode Close() noexcept { return thread_.Close(); }

        /// <summary>Waits until the thread has exited.</summary>
        /// <param name="milliseconds">Timeout in milliseconds; <c>INFINITE</c> waits without one.</param>
        /// <returns>
        /// <see cref="Error::Success"/> if the thread has exited, <see cref="Error::WaitTimedOut"/> if the wait
        /// timed out, otherwise the Win32 error of the wait.
        /// </returns>
        /// <remarks>
        /// Blocks the calling OS thread: called from a task it blocks the whole scheduler thread.
        /// </remarks>
        /// <seealso cref="Create"/>
        ErrorCode Wait(DWORD milliseconds) noexcept
        {
            DWORD     event = WAIT_FAILED;
            ErrorCode error = thread_.Wait(milliseconds, event);
            if (Error::Succeeded(error) && event != WAIT_OBJECT_0)
                error = Error::WaitTimedOut;
            return error;
        }

    protected:
        /// <summary>Constructs the object; no thread runs until <see cref="Create"/>.</summary>
        ConvenienceThread() noexcept = default;

        /// <summary>Closes the thread handle.</summary>
        /// <remarks>Does not wait for the thread.</remarks>
        virtual ~ConvenienceThread() noexcept = default;

        /// <summary>The thread's body.</summary>
        /// <remarks>Called once on the new thread; the thread ends when it returns.</remarks>
        virtual void Run() noexcept = 0;

    private:
        static DWORD WINAPI ThreadProc(void* lpv) noexcept
        {
            CT_ASSERT(lpv);

            auto thread = static_cast<ConvenienceThread*>(lpv);
            thread->Run();

            return 0;
        }

        Thread thread_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows