//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Base classes of all asynchronous I/O requests: the platform's request structure (an OVERLAPPED
// on Windows, the core's IoRequest elsewhere) plus the completion callback the core's I/O thread
// calls, and the completion routine (AsyncIoRequestCompleted) that hands a completion to its
// request. File and socket requests derive from these.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
#if defined(CT_PLATFORM_WINDOWS)
    using _NativeIoRequest  = OVERLAPPED;
    using _CompletionError = DWORD; // what the core's IoCompletionRoutine reports
#else
    using _NativeIoRequest  = IoRequest;
    using _CompletionError = int;
#endif

    /// <summary>One asynchronous I/O operation.</summary>
    /// <remarks>
    /// Owns the platform's request structure - the <c>OVERLAPPED</c> passed to the Win32 call on
    /// Windows, the <see cref="IoRequest"/> passed to <see cref="SubmitIo"/> elsewhere - and is told
    /// the result through <see cref="RequestCompleted"/>. While an operation is pending, the request
    /// and the buffer of the operation must stay alive: the kernel works with them and the I/O thread
    /// reports the result to the request. The request does not own the buffer. A request carries one
    /// operation at a time; call <see cref="Reset"/> before reusing it for the next one.
    /// </remarks>
    /// <seealso cref="AsyncIoRequest"/>
    /// <seealso cref="AsyncIoRequestCompleted"/>
    // The platform's structure is a private base, so the completion can map the pointer the core
    // returns back to the request.
    export class CT_NOVTABLE AsyncIoRequestBase : private _NativeIoRequest
    {
        AsyncIoRequestBase(const AsyncIoRequestBase&)            = delete;
        AsyncIoRequestBase(AsyncIoRequestBase&&)                 = delete;
        AsyncIoRequestBase& operator=(const AsyncIoRequestBase&) = delete;
        AsyncIoRequestBase& operator=(AsyncIoRequestBase&&)      = delete;

    public:
        /// <summary>A byte count of one operation, on every platform.</summary>
        using ByteCount = std::uint32_t;

        /// <summary>The error code a completion reports: a Win32 error code on Windows, an <c>errno</c> elsewhere.</summary>
        using CompletionError = _CompletionError;

    protected:
        /// <summary>Creates an empty request.</summary>
        /// <remarks>The request structure is zeroed (offset <c>0</c>) and no bytes are transferred.</remarks>
        AsyncIoRequestBase() noexcept { ResetInternal(); }

        /// <summary>Stores the transferred byte count; for use by <see cref="RequestCompleted"/>.</summary>
        /// <param name="transferredBytes">Bytes the operation transferred.</param>
        void SetTransferredBytes(ByteCount transferredBytes) noexcept { transferredBytes_ = transferredBytes; }

    public:
        /// <summary>Maps the platform's request structure back to its request.</summary>
        /// <param name="request">
        /// An <c>OVERLAPPED</c> (Windows) or <see cref="IoRequest"/> (elsewhere) that belongs to a request, as the
        /// core hands it to the completion routine.
        /// </param>
        /// <returns>The request that contains <paramref name="request"/>.</returns>
        /// <remarks>Pass only a structure obtained from <c>GetOverlapped</c> or <c>GetIoRequest</c> of a request.</remarks>
        /// <seealso cref="AsyncIoRequestCompleted"/>
        static AsyncIoRequestBase* FromNativeRequest(_NativeIoRequest* request) noexcept { return static_cast<AsyncIoRequestBase*>(request); }

        /// <summary>Bytes the completed operation transferred (as stored by <see cref="RequestCompleted"/>).</summary>
        /// <returns>The transferred bytes; <c>0</c> before completion and after <see cref="Reset"/>.</returns>
        ByteCount GetTransferredBytes() const noexcept { return transferredBytes_; }

        /// <summary>Completes the request on the calling thread when the call succeeded at once.</summary>
        /// <param name="transferredBytes">Bytes the call reported as transferred.</param>
        /// <remarks>
        /// The core associates every handle with <c>FILE_SKIP_COMPLETION_PORT_ON_SUCCESS</c>, so an
        /// operation that succeeds immediately posts no completion packet: the issuer must
        /// complete the request itself, which is what this does (<see cref="RequestCompleted"/> with
        /// <c>0</c>, success). Do not call it for a pending operation - that one is completed by the
        /// I/O thread. Outside Windows every submitted operation completes on the I/O thread, so
        /// only an issuer that does the work itself has a use for it.
        /// </remarks>
        /// <seealso cref="RequestCompleted"/>
        void OnRequestCompletedImmediately(ByteCount transferredBytes) noexcept { RequestCompleted(0, transferredBytes); }

        /// <summary>Called exactly once per operation with its result.</summary>
        /// <param name="error">
        /// The operation's error code; <c>0</c> when it succeeded. A Win32 error code on Windows, an
        /// <c>errno</c> elsewhere.
        /// </param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>
        /// Runs on the core's single I/O thread for a pending operation, or on the issuing thread
        /// through <see cref="OnRequestCompletedImmediately"/> (or when the issuer reports an immediate failure
        /// itself). Keep it short and do not block or suspend in it: it holds up every other
        /// completion of the process. A waiter may destroy the request as soon as it is
        /// signalled, so signalling must be the last access to the request.
        /// </remarks>
        /// <seealso cref="AsyncIoRequestCompleted"/>
        virtual void RequestCompleted(CompletionError error, ByteCount bytesTransferred) noexcept = 0;

        /// <summary>Prepares the request for the next operation.</summary>
        /// <remarks>
        /// Zeroes the request structure (including the offset) and the transferred bytes. Only while no
        /// operation is pending.
        /// </remarks>
        virtual void Reset() noexcept { ResetInternal(); }

#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>The <c>OVERLAPPED</c> to pass to the Win32 call (<c>ReadFile</c>, <c>WSARecv</c>, ...).</summary>
        /// <returns>The request's own <c>OVERLAPPED</c>; it lives as long as the request.</returns>
        OVERLAPPED& GetOverlapped() noexcept { return *this; }

        /// <summary>The <c>OVERLAPPED</c> to pass to the Win32 call (<c>ReadFile</c>, <c>WSARecv</c>, ...).</summary>
        /// <returns>The request's own <c>OVERLAPPED</c>; it lives as long as the request.</returns>
        const OVERLAPPED& GetOverlapped() const noexcept { return *this; }
#else
        /// <summary>The <see cref="IoRequest"/> to fill and pass to <see cref="SubmitIo"/>.</summary>
        /// <returns>The request's own IoRequest; it lives as long as the request.</returns>
        IoRequest& GetIoRequest() noexcept { return *this; }

        /// <summary>The <see cref="IoRequest"/> to fill and pass to <see cref="SubmitIo"/>.</summary>
        /// <returns>The request's own IoRequest; it lives as long as the request.</returns>
        const IoRequest& GetIoRequest() const noexcept { return *this; }
#endif

    private:
        void ResetInternal() noexcept
        {
            std::memset(static_cast<_NativeIoRequest*>(this), 0, sizeof(_NativeIoRequest));
            transferredBytes_ = 0;
        }

        ByteCount transferredBytes_;
    };

    /// <summary>A request that also stores the operation's error code.</summary>
    /// <typeparam name="ErrorType">
    /// Integer error code type of the API; <c>0</c> must mean success.
    /// </typeparam>
    /// <remarks>
    /// The error code has the type of its API: <see cref="Platform::ErrorCode"/> (a Win32 error on
    /// Windows, an <c>errno</c> elsewhere) for files, the Winsock error type for sockets. <c>0</c>
    /// means success.
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

#if defined(CT_PLATFORM_WINDOWS)
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
        AsyncIoRequestBase::FromNativeRequest(overlapped)->RequestCompleted(error, bytesTransferred);
    }
#else
    /// <summary>The completion routine of every <see cref="IoRequest"/> that belongs to an <see cref="AsyncIoRequestBase"/>.</summary>
    /// <param name="request">The completed operation's IoRequest; must belong to an AsyncIoRequestBase.</param>
    /// <param name="result">What the core reports: the bytes transferred, or the negated <c>errno</c>.</param>
    /// <remarks>
    /// Store it in <see cref="IoRequest::routine"/> before <see cref="SubmitIo"/>. Runs on the core's
    /// I/O thread and calls the request's <see cref="AsyncIoRequestBase::RequestCompleted"/> with the
    /// <c>errno</c> (<c>0</c> on success) and the bytes transferred.
    /// </remarks>
    /// <seealso cref="EnableAsyncIo"/>
    export inline void AsyncIoRequestCompleted(IoRequest* request, std::int32_t result) noexcept
    {
        // The core reports bytes, or a negated errno; the request gets them apart, as on Windows.
        const bool failed = result < 0;
        AsyncIoRequestBase::FromNativeRequest(request)->RequestCompleted(failed ? -result : 0, failed ? 0 : static_cast<std::uint32_t>(result));
    }

    /// <summary>Cancels a submitted operation (<see cref="CancelIo"/>), trying again while the kernel takes nothing.</summary>
    /// <param name="request">A request <see cref="SubmitIo"/> accepted.</param>
    /// <remarks>
    /// For the waiting forms, which wait for the completion without a limit after the cancel: a cancel the
    /// kernel refused (<c>EAGAIN</c>, <c>EBUSY</c>) would leave an operation that does not end on its own,
    /// such as a receive with nothing to read, pending for good. Task only: yields between the tries.
    /// Gives up on any other error; <c>ENODEV</c> (asynchronous I/O disabled meanwhile) is a misuse and
    /// asserted.
    /// </remarks>
    export inline void _CancelIoRetrying(IoRequest* request) noexcept
    {
        while (!CancelIo(request)) {
            const int error = errno;
            CT_ASSERT(error == EAGAIN || error == EBUSY);
            if (error != EAGAIN && error != EBUSY)
                return;
            YieldTask();
        }
    }

    /// <summary>Whether an operation that was cancelled after a timeout ended as cancelled.</summary>
    /// <param name="error">The operation's <c>errno</c>.</param>
    /// <returns>
    /// <c>true</c> for <c>ECANCELED</c>, and for <c>EINTR</c>, which an operation the kernel was already carrying
    /// out reports when it is cancelled.
    /// </returns>
    export constexpr bool _EndedByCancel(int error) noexcept { return error == ECANCELED || error == EINTR; }
#endif

    /// <summary>The result of an operation that was cancelled because its wait ended early.</summary>
    /// <typeparam name="Error">The error names of the operation's API (<c>Platform::Error</c>, <c>Platform::SocketError</c>).</typeparam>
    /// <param name="error">The error the operation completed with after the cancel.</param>
    /// <param name="status">How the wait ended: <see cref="WaitStatus::TimedOut"/> or <see cref="WaitStatus::Cancelled"/>.</param>
    /// <returns>
    /// <c>Error::TimedOut</c> after a timeout and <c>Error::OperationAborted</c> after a cancel of the task, if the
    /// cancel ended the operation; otherwise <paramref name="error"/>: it completed meanwhile, and its result stands.
    /// </returns>
    export template <typename Error>
    constexpr auto _ErrorAfterCancel(typename Error::Type error, WaitStatus status) noexcept -> typename Error::Type
    {
#if defined(CT_PLATFORM_WINDOWS)
        return error == Error::OperationAborted && status == WaitStatus::TimedOut ? Error::TimedOut : error;
#else
        // ECANCELED, or EINTR from an operation the kernel was already carrying out
        if (!_EndedByCancel(error))
            return error;
        return status == WaitStatus::TimedOut ? Error::TimedOut : Error::OperationAborted;
#endif
    }

    /// <summary>Starts an operation, again while the kernel takes no more for the moment.</summary>
    /// <typeparam name="Start">A callable that starts the operation and returns its error code.</typeparam>
    /// <param name="start">Starts the operation; called again after each refusal.</param>
    /// <returns>What the last start returned.</returns>
    /// <remarks>
    /// Outside Windows a refused submission (<c>EAGAIN</c>, <c>EBUSY</c>: completions waiting for room,
    /// memory pressure) passes: the calling task yields and tries again, instead of taking it for the
    /// operation's failure. On Windows the start is made once, as before. Task only.
    /// </remarks>
    export template <typename Start>
    inline auto _StartRetrying(Start start) noexcept
    {
        auto result = start();
#if !defined(CT_PLATFORM_WINDOWS)
        while (result == EAGAIN || result == EBUSY) {
            YieldTask();
            result = start();
        }
#endif
        return result;
    }
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo
