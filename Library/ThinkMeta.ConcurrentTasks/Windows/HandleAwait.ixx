//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Await: waits until a Windows object (event, process, thread, change notification, ...) is
// signaled, by suspending the calling task instead of blocking its thread.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Windows.Await;

export import ThinkMeta.ConcurrentTasks.Windows.Error;
import ThinkMeta.ConcurrentTasks.Synchronization.Await;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Waits until a Windows object is signaled.</summary>
    /// <param name="handle">
    /// A handle that <c>WaitForSingleObject</c> accepts: event, semaphore, mutex, process, thread,
    /// waitable timer, change notification and others.
    /// </param>
    /// <returns><see cref="Error::Success"/>, or the error of creating the thread-pool wait.</returns>
    /// <remarks>
    /// Returns at once if the object is signaled, otherwise suspends the calling task until it is;
    /// the scheduler thread runs other tasks meanwhile. A thread-pool wait watches the object.
    /// Waiting consumes the signal as <c>WaitForSingleObject</c> does: an auto-reset event is reset,
    /// a semaphore count taken. A mutex is acquired by a thread of the thread pool, not by the
    /// caller; do not use this function for mutexes.
    /// Task or converted thread only. Keep the handle open until the function returns. There is no
    /// timed variant.
    /// </remarks>
    /// <seealso cref="Synchronization::Await"/>
    export inline ErrorCode Await(HANDLE handle) noexcept
    {
        if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0)
            return Error::Success;

        auto callback
            = [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT) noexcept { (*static_cast<Synchronization::Completion*>(context))(); };

        PTP_WAIT  wait  = nullptr;
        ErrorCode error = Error::Success;
        Synchronization::Await([&](Synchronization::Completion& completion) noexcept {
            wait = CreateThreadpoolWait(callback, &completion, nullptr);
            if (wait == nullptr) {
                error = Error::GetLastError();
                completion();
                return;
            }
            SetThreadpoolWait(wait, handle, nullptr);
        });

        // The callback has called the completion; the thread pool frees the wait once it has returned.
        if (wait != nullptr)
            CloseThreadpoolWait(wait);
        return error;
    }
} // namespace ThinkMeta::ConcurrentTasks::Windows
