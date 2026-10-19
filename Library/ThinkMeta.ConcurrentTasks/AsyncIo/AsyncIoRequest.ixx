//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Base classes of all overlapped I/O requests: an OVERLAPPED plus the completion callback the
// core's I/O thread calls, and the completion routine (AsyncIoRequestCompleted) that handles are
// associated with. File and socket requests derive from these.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    /// <summary>One overlapped I/O operation.</summary>
    /// <remarks>
    /// Owns the <c>OVERLAPPED</c> that is passed to the Win32 call and is told the result through
    /// <see cref="RequestCompleted"/>. While an operation is pending, the request and the buffer of the operation must stay alive:
    /// the kernel writes into the <c>OVERLAPPED</c> and the I/O thread reports the result to the request.
    /// The request does not own the buffer. A request carries one operation at a time; call <see cref="Reset"/>
    /// before reusing it for the next one.
    /// </remarks>
    /// <seealso cref="AsyncIoRequest"/>
    /// <seealso cref="AsyncIoRequestCompleted"/>
    // The OVERLAPPED is a private base, so FromOverlapped can map the pointer the completion port
    // returns back to the request.
    export class CT_NOVTABLE AsyncIoRequestBase : private OVERLAPPED
    {
        AsyncIoRequestBase(const AsyncIoRequestBase&)            = delete;
        AsyncIoRequestBase(AsyncIoRequestBase&&)                 = delete;
        AsyncIoRequestBase& operator=(const AsyncIoRequestBase&) = delete;
        AsyncIoRequestBase& operator=(AsyncIoRequestBase&&)      = delete;

    protected:
        /// <summary>Creates an empty request.</summary>
        /// <remarks>The <c>OVERLAPPED</c> is zeroed (offset <c>0</c>) and no bytes are transferred.</remarks>
        AsyncIoRequestBase() noexcept { ResetInternal(); }

        /// <summary>Stores the transferred byte count; for use by <see cref="RequestCompleted"/>.</summary>
        /// <param name="transferredBytes">Bytes the operation transferred.</param>
        void SetTransferredBytes(DWORD transferredBytes) noexcept { transferredBytes_ = transferredBytes; }

    public:
        /// <summary>Maps an <c>OVERLAPPED</c> back to its request.</summary>
        /// <param name="overlapped">An <c>OVERLAPPED</c> that belongs to a request, e.g. as dequeued from the port.</param>
        /// <returns>The request that contains <paramref name="overlapped"/>.</returns>
        /// <remarks>Pass only an <c>OVERLAPPED</c> obtained from <see cref="GetOverlapped"/> of a request.</remarks>
        /// <seealso cref="AsyncIoRequestCompleted"/>
        static AsyncIoRequestBase* FromOverlapped(OVERLAPPED* overlapped) noexcept { return static_cast<AsyncIoRequestBase*>(overlapped); }

        /// <summary>Bytes the completed operation transferred (as stored by <see cref="RequestCompleted"/>).</summary>
        /// <returns>The transferred bytes; <c>0</c> before completion and after <see cref="Reset"/>.</returns>
        DWORD GetTransferredBytes() const noexcept { return transferredBytes_; }

        /// <summary>Completes the request on the calling thread when the Win32 call succeeded at once.</summary>
        /// <param name="transferredBytes">Bytes the Win32 call reported as transferred.</param>
        /// <remarks>
        /// The core associates every handle with <c>FILE_SKIP_COMPLETION_PORT_ON_SUCCESS</c>, so an
        /// operation that succeeds immediately posts no completion packet: the issuer must
        /// complete the request itself, which is what this does (<see cref="RequestCompleted"/> with
        /// <c>ERROR_SUCCESS</c>). Do not call it for a pending operation (<c>ERROR_IO_PENDING</c>) - that one is
        /// completed by the I/O thread.
        /// </remarks>
        /// <seealso cref="RequestCompleted"/>
        void OnRequestCompletedImmediately(DWORD transferredBytes) noexcept { RequestCompleted(ERROR_SUCCESS, transferredBytes); }

        /// <summary>Called exactly once per operation with its result.</summary>
        /// <param name="error">The operation's Win32 error code; <c>ERROR_SUCCESS</c> when it succeeded.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>
        /// Runs on the core's single I/O thread for a pending operation, or on the issuing thread
        /// through <see cref="OnRequestCompletedImmediately"/> (or when the issuer reports an immediate failure
        /// itself). Keep it short and do not block or suspend in it: it holds up every other
        /// completion of the process. A waiter may destroy the request as soon as it is
        /// signalled, so signalling must be the last access to the request.
        /// </remarks>
        /// <seealso cref="AsyncIoRequestCompleted"/>
        virtual void RequestCompleted(DWORD error, DWORD bytesTransferred) noexcept = 0;

        /// <summary>Prepares the request for the next operation.</summary>
        /// <remarks>
        /// Zeroes the <c>OVERLAPPED</c> (including the offset) and the transferred bytes. Only while no
        /// operation is pending.
        /// </remarks>
        virtual void Reset() noexcept { ResetInternal(); }

        /// <summary>The <c>OVERLAPPED</c> to pass to the Win32 call (<c>ReadFile</c>, <c>WSARecv</c>, ...).</summary>
        /// <returns>The request's own <c>OVERLAPPED</c>; it lives as long as the request.</returns>
        OVERLAPPED& GetOverlapped() noexcept { return *this; }

        /// <summary>The <c>OVERLAPPED</c> to pass to the Win32 call (<c>ReadFile</c>, <c>WSARecv</c>, ...).</summary>
        /// <returns>The request's own <c>OVERLAPPED</c>; it lives as long as the request.</returns>
        const OVERLAPPED& GetOverlapped() const noexcept { return *this; }

    private:
        void ResetInternal() noexcept
        {
            ZeroMemory(static_cast<OVERLAPPED*>(this), sizeof(OVERLAPPED));
            transferredBytes_ = 0;
        }

        DWORD transferredBytes_;
    };

    /// <summary>A request that also stores the operation's error code.</summary>
    /// <typeparam name="ErrorType">
    /// Integer error code type of the API; <c>0</c> must mean success.
    /// </typeparam>
    /// <remarks>
    /// The error code has the type of its API: <see cref="Windows::ErrorCode"/> (Win32 error) for files, the
    /// Winsock error type for sockets. <c>0</c> means success.
    /// </remarks>
    /// <seealso cref="AsyncFileIoRequest"/>
    export template <typename ErrorType>
    class CT_NOVTABLE AsyncIoRequest : public AsyncIoRequestBase
    {
        AsyncIoRequest(const AsyncIoRequest&)            = delete;
        AsyncIoRequest(AsyncIoRequest&&)                 = delete;
        AsyncIoRequest& operator=(const AsyncIoRequest&) = delete;
        AsyncIoRequest& operator=(AsyncIoRequest&&)      = delete;

    public:
        /// <summary>The completed operation's error code (<c>0</c>: success, also before completion).</summary>
        /// <returns>The error code; <c>0</c> if the operation succeeded or has not completed yet.</returns>
        ErrorType GetError() const noexcept { return error_; }

        /// <summary>Like <see cref="AsyncIoRequestBase::Reset"/>, and also clears the error code.</summary>
        void Reset() noexcept override
        {
            AsyncIoRequestBase::Reset();
            ResetInternal();
        }

    protected:
        /// <summary>Creates a request with error code <c>0</c>.</summary>
        AsyncIoRequest() noexcept { ResetInternal(); }

        /// <summary>Stores the error code; for use by <see cref="AsyncIoRequestBase::RequestCompleted"/>.</summary>
        /// <param name="error">The operation's error code; <c>0</c> for success.</param>
        void SetError(ErrorType error) noexcept { error_ = error; }

    private:
        void ResetInternal() noexcept { error_ = 0; }

        ErrorType error_;
    };

    /// <summary>The completion routine for handles whose operations use <see cref="AsyncIoRequestBase"/> requests.</summary>
    /// <param name="overlapped">The completed operation's <c>OVERLAPPED</c>; must belong to an AsyncIoRequestBase.</param>
    /// <param name="error">The operation's Win32 error code; <c>ERROR_SUCCESS</c> when it succeeded.</param>
    /// <param name="bytesTransferred">Bytes the operation transferred.</param>
    /// <remarks>
    /// Pass it as the <see cref="IoCompletionRoutine"/> (see Core.h) to <see cref="AssociateHandleWithIoCompletionPort"/>.
    /// Runs on the core's I/O thread and calls the request's <see cref="AsyncIoRequestBase::RequestCompleted"/> with the
    /// operation's own error code. Every <c>OVERLAPPED</c> issued on such a handle must belong to an
    /// AsyncIoRequestBase.
    /// </remarks>
    /// <seealso cref="EnableAsyncIo"/>
    export inline void AsyncIoRequestCompleted(OVERLAPPED* overlapped, DWORD error, DWORD bytesTransferred) noexcept
    {
        // Its address travels to the core as the handle's completion key, so the core needs no
        // knowledge of the request classes; the OVERLAPPED leads back to its request.
        AsyncIoRequestBase::FromOverlapped(overlapped)->RequestCompleted(error, bytesTransferred);
    }
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo
