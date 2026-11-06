//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Requests for asynchronous file I/O (AsyncFile): the file offset in the request structure, the
// platform's error code, and waitable variants whose completion a task can wait for.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#endif

#include <concepts>
#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncFileIoRequest;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
import ThinkMeta.ConcurrentTasks.Platform.Error;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    using Synchronization::_ManualResetEvent;
    using Synchronization::SuspensionKind;

    /// <summary>An asynchronous file read or write.</summary>
    /// <remarks>
    /// Holds the file offset to transfer at, and after completion the platform's error code
    /// (<see cref="GetError"/>, compare it through <see cref="Platform::Error"/>) and the transferred
    /// bytes (<see cref="GetTransferredBytes"/>). A read that starts at or beyond the end of the
    /// file completes with <see cref="Platform::Error::EndOfFile"/> on every platform.
    /// Not usable on its own: use <see cref="WaitableAsyncFileIoRequest"/>(_s), or an own request type
    /// built on it that reacts to the completion in <see cref="RequestCompleted"/>. The request must stay
    /// alive while its operation is pending (see <see cref="AsyncIoRequestBase"/>).
    /// </remarks>
    /// <seealso cref="WaitableAsyncFileIoRequest_s"/>
    /// <seealso cref="AsyncFile"/>
    export class CT_NOVTABLE AsyncFileIoRequest : public AsyncIoRequest<Platform::ErrorCode>
    {
        AsyncFileIoRequest(const AsyncFileIoRequest&)            = delete;
        AsyncFileIoRequest(AsyncFileIoRequest&&)                 = delete;
        AsyncFileIoRequest& operator=(const AsyncFileIoRequest&) = delete;
        AsyncFileIoRequest& operator=(AsyncFileIoRequest&&)      = delete;

    public:
        /// <summary>The 64-bit file offset of the operation.</summary>
        /// <returns>The file offset in bytes; <c>0</c> after <see cref="Reset"/>.</returns>
        /// <remarks>Stored in <c>OVERLAPPED</c> <c>Offset</c>/<c>OffsetHigh</c> on Windows, in <see cref="IoRequest::offset"/> elsewhere.</remarks>
        /// <seealso cref="SetOffset"/>
        [[nodiscard]] std::uint64_t GetOffset() const noexcept
        {
#if defined(CT_PLATFORM_WINDOWS)
            auto& overlapped = GetOverlapped();
            return (static_cast<std::uint64_t>(overlapped.OffsetHigh) << 32) | overlapped.Offset;
#else
            return GetIoRequest().offset;
#endif
        }

        /// <summary>Sets the file offset for the next operation.</summary>
        /// <param name="offset">
        /// File offset in bytes to read or write at; sector-aligned for a file opened unbuffered
        /// (<c>FILE_FLAG_NO_BUFFERING</c>, <c>O_DIRECT</c>).
        /// </param>
        /// <remarks>
        /// Call after <see cref="Reset"/> (which zeroes it) and before issuing; the asynchronous
        /// operations use no file pointer, every operation needs one.
        /// </remarks>
        /// <seealso cref="GetOffset"/>
        void SetOffset(std::uint64_t offset) noexcept
        {
#if defined(CT_PLATFORM_WINDOWS)
            auto& overlapped = GetOverlapped();

            overlapped.Offset     = static_cast<DWORD>(offset);
            overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
#else
            GetIoRequest().offset = offset;
#endif
        }

        /// <summary>Stores the error code and the transferred bytes; signals nothing.</summary>
        /// <param name="error">The operation's error code; <c>0</c> when it succeeded.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>
        /// Outside Windows a read of <c>0</c> bytes that asked for more is stored as
        /// <see cref="Platform::Error::EndOfFile"/>: Linux reports the end of a file that way, Windows
        /// as <c>ERROR_HANDLE_EOF</c>.
        /// </remarks>
        void RequestCompleted(CompletionError error, ByteCount bytesTransferred) noexcept override
        {
#if !defined(CT_PLATFORM_WINDOWS)
            const auto& request = GetIoRequest(); // the IoRequest, a private base
            if (error == Platform::Error::Success && bytesTransferred == 0 && request.operation == IoOperation::Read && request.length != 0)
                error = Platform::Error::EndOfFile;
#endif
            SetError(error);
            SetTransferredBytes(bytesTransferred);
        }

    protected:
        /// <summary>Creates a request at file offset <c>0</c>.</summary>
        AsyncFileIoRequest() noexcept = default;
    };

    /// <summary>A file request whose completion a task can wait for.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of tasks may wait; <see cref="SuspensionKind::Single"/>: at
    /// most one waiting task, cheaper, allows a timeout.
    /// </typeparam>
    /// <remarks>
    /// A manual-reset event is set when the operation completes. Use <see cref="WaitableAsyncFileIoRequest"/>
    /// (any number of waiting tasks) or <see cref="WaitableAsyncFileIoRequest_s"/> (at most one waiting task).
    /// <see cref="Wait"/> suspends the calling task (its thread runs other tasks meanwhile); a plain OS thread
    /// must first call <see cref="ConvertThreadToTask"/> (Core.h). The event is set on the completing thread
    /// (I/O thread, or the issuer for an immediate completion), as the last access of the
    /// completion: with <see cref="WaitableAsyncFileIoRequest_s"/>, once Wait returns, the request may be
    /// destroyed at once, which allows requests in a task's stack frame. With
    /// <see cref="WaitableAsyncFileIoRequest"/> it may not: its event stores the signal and then takes
    /// the waiters, so the request must stay until the completion has returned and no task waits on
    /// it any more (see <see cref="Synchronization::ManualResetEvent"/>); a request that one task
    /// waits for, as in a reader or writer loop, is a case for the _s variant. An operation that failed to be issued (an error other than
    /// <see cref="Platform::Error::Pending"/>) never completes on its own: do not wait for it, or complete it with
    /// <see cref="RequestCompleted"/> yourself. Never destroy the request while an operation is pending.
    /// </remarks>
    /// <seealso cref="AsyncFileIoRequest"/>
    template <SuspensionKind _SuspensionKind>
    class _WaitableAsyncFileIoRequest : public AsyncFileIoRequest
    {
    public:
        /// <summary>Stores the result, then sets the event (the last access to the request).</summary>
        /// <param name="error">The operation's error code; <c>0</c> when it succeeded.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        void RequestCompleted(CompletionError error, ByteCount bytesTransferred) noexcept override
        {
            AsyncFileIoRequest::RequestCompleted(error, bytesTransferred);
            event_.Set();
        }

        /// <summary>Prepares the request for the next operation and resets the event.</summary>
        /// <remarks>Not while an operation is pending.</remarks>
        void Reset() noexcept override
        {
            AsyncFileIoRequest::Reset();
            event_.Reset();
        }

        /// <summary><c>true</c> if the operation has completed; never suspends.</summary>
        /// <returns><c>true</c> if the operation has completed; <c>false</c> if it is still pending.</returns>
        /// <seealso cref="Wait"/>
        bool TryWait() noexcept { return event_.TryWait(); }

        /// <summary>Suspends the calling task until the operation has completed.</summary>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept { event_.Wait(); }

        /// <summary>Suspends the calling task until the operation has completed or the timeout has passed.</summary>
        /// <param name="timeout">How long to wait at most.</param>
        /// <returns>
        /// <see cref="WaitStatus::Signaled"/> if the operation has completed;
        /// <see cref="WaitStatus::TimedOut"/> if it timed out; <see cref="WaitStatus::Cancelled"/> if
        /// the waiting task was cancelled (<see cref="CancelTask"/>).
        /// </returns>
        /// <remarks>
        /// <see cref="WaitableAsyncFileIoRequest_s"/> only. Needs <see cref="EnableTimers"/>, else the wait has no limit. A timeout or cancel does not cancel the
        /// operation: it stays pending and the request must stay alive until it completes (cancel
        /// it - <c>CancelIoEx</c> on Windows, <see cref="CancelIo"/> elsewhere - and Wait, as <see cref="AsyncFile::Read"/> does).
        /// </remarks>
        /// <seealso cref="TryWait"/>
        [[nodiscard]] WaitStatus Wait(Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            return event_.Wait(timeout);
        }

    private:
        _ManualResetEvent<_SuspensionKind> event_;
    };

    /// <summary>Waitable file request on which any number of tasks may wait.</summary>
    /// <seealso cref="WaitableAsyncFileIoRequest_s"/>
    export using WaitableAsyncFileIoRequest = _WaitableAsyncFileIoRequest<SuspensionKind::Multiple>;

    /// <summary>Waitable file request with at most one waiting task.</summary>
    /// <remarks>Cheaper, and allows <see cref="_WaitableAsyncFileIoRequest::Wait"/> with a timeout.</remarks>
    /// <seealso cref="WaitableAsyncFileIoRequest"/>
    export using WaitableAsyncFileIoRequest_s = _WaitableAsyncFileIoRequest<SuspensionKind::Single>;

    /// <summary>Satisfied by the two waitable file request variants.</summary>
    /// <typeparam name="T">
    /// The type to test: <see cref="WaitableAsyncFileIoRequest"/> or <see cref="WaitableAsyncFileIoRequest_s"/>.
    /// </typeparam>
    /// <seealso cref="WaitableAsyncFileIoRequest"/>
    /// <seealso cref="WaitableAsyncFileIoRequest_s"/>
    export template <typename T>
    concept WaitableAsyncFileIoRequestType = std::same_as<T, WaitableAsyncFileIoRequest> || std::same_as<T, WaitableAsyncFileIoRequest_s>;
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo