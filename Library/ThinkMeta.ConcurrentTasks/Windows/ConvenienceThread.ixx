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
        /// <returns><c>true</c> if the thread was started; <c>false</c> on failure (<c>GetLastError</c> has the reason).</returns>
        /// <remarks>Call once.</remarks>
        /// <seealso cref="Wait"/>
        bool Create(size_t stackSize = 0) noexcept { return Error::Succeeded(thread_.Create(nullptr, stackSize, ThreadProc, this, 0, nullptr)); }

        /// <summary>Returns whether the thread handle is open (created and not closed).</summary>
        /// <returns><c>true</c> if the handle is open; otherwise <c>false</c>.</returns>
        bool IsValid() const noexcept { return thread_.IsValid(); }

        /// <summary>Closes the thread handle.</summary>
        /// <returns>
        /// <c>true</c> on success, also if no handle was open; <c>false</c> on failure (<c>GetLastError</c>
        /// has the reason).
        /// </returns>
        /// <remarks>The thread keeps running; <see cref="Wait"/> is no longer possible afterwards.</remarks>
        bool Close() noexcept { return Error::Succeeded(thread_.Close()); }

        /// <summary>Waits until the thread has exited.</summary>
        /// <param name="milliseconds">Timeout in milliseconds; <c>INFINITE</c> waits without one.</param>
        /// <returns>
        /// <c>true</c> if the thread has exited, <c>false</c> if the wait timed out or failed.
        /// </returns>
        /// <remarks>
        /// Blocks the calling OS thread: called from a task it blocks the whole scheduler thread.
        /// </remarks>
        /// <seealso cref="Create"/>
        bool Wait(DWORD milliseconds) noexcept
        {
            DWORD event;
            return Error::Succeeded(thread_.Wait(milliseconds, event)) && event == WAIT_OBJECT_0;
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
        static DWORD WINAPI ThreadProc(LPVOID lpv) noexcept
        {
            CT_ASSERT(lpv);

            auto thread = static_cast<ConvenienceThread*>(lpv);
            thread->Run();

            return 0;
        }

        Thread thread_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows