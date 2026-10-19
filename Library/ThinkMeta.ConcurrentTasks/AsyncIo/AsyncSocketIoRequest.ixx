//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The request objects of AsyncSocket operations: AsyncSocketIoRequest carries the OVERLAPPED and
// the result (WinsockErrorCode, bytes) of one operation; the Waitable variants add an event a
// task can wait on, and AsyncSocketRecvIoRequest also captures the receive result flags.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>

#include <concepts>
#include <utility>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocketIoRequest;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
import ThinkMeta.ConcurrentTasks.Windows.WinsockError;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    using Synchronization::_ManualResetEvent;
    using Synchronization::SuspensionKind;

    /// <summary>The state of one overlapped socket operation.</summary>
    /// <remarks>
    /// Holds its <c>OVERLAPPED</c>, the bytes transferred and the <see cref="Windows::WinsockErrorCode"/> it
    /// completed with. Not usable on its own: use a Waitable variant below (<see cref="WaitableAsyncSocketIoRequest"/>,
    /// <see cref="WaitableAsyncSocketIoRequest_s"/>), or an own request type that reacts to the completion
    /// in <see cref="RequestCompleted"/>. The owner keeps the request alive and in place from the start of
    /// the operation until RequestCompleted has run; it can be reused for another operation after
    /// <see cref="AsyncIoRequest::Reset"/>.
    /// </remarks>
    /// <seealso cref="AsyncSocket"/>
    export class CT_NOVTABLE AsyncSocketIoRequest : public AsyncIoRequest<Windows::WinsockErrorCode>
    {
        AsyncSocketIoRequest(const AsyncSocketIoRequest&)            = delete;
        AsyncSocketIoRequest(AsyncSocketIoRequest&&)                 = delete;
        AsyncSocketIoRequest& operator=(const AsyncSocketIoRequest&) = delete;
        AsyncSocketIoRequest& operator=(AsyncSocketIoRequest&&)      = delete;

    public:
        /// <summary>Records the operation's result.</summary>
        /// <param name="error">The operation's own Win32/WSA error code; <c>ERROR_SUCCESS</c> (<c>0</c>) on success.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>
        /// Called exactly once per operation: on the starting thread when the operation completed
        /// synchronously, otherwise on the core's I/O thread. Completion handling in an own request type
        /// must be short and must not block or suspend.
        /// </remarks>
        void RequestCompleted(DWORD error, DWORD bytesTransferred) noexcept override
        {
            SetTransferredBytes(bytesTransferred);
            // A failed packet carries the Win32 form of the operation's NTSTATUS (ERROR_NETNAME_DELETED
            // for a reset), not the WSAE* code a synchronous failure reports; the status in the
            // OVERLAPPED gives the Winsock code back (see WinsockError::FromOverlapped).
            SetError(Windows::WinsockError::FromOverlapped(GetOverlapped(), static_cast<Windows::WinsockErrorCode>(error)));
        }

    protected:
        /// <summary>Creates an idle request.</summary>
        /// <remarks>Zeroed <c>OVERLAPPED</c>, no error, <c>0</c> bytes.</remarks>
        AsyncSocketIoRequest() noexcept = default;
    };

    /// <summary>A request for receive operations that also captures the result flags.</summary>
    /// <remarks>
    /// The flags (<c>MSG_PARTIAL</c>, <c>MSG_TRUNC</c>, ...) are available once the operation has completed
    /// (<see cref="GetFlags"/>). Not usable on its own: use <see cref="WaitableAsyncSocketRecvIoRequest"/>(_s), or an own request
    /// type built on it.
    /// </remarks>
    /// <seealso cref="WaitableAsyncSocketRecvIoRequest_s"/>
    export class CT_NOVTABLE AsyncSocketRecvIoRequest : public AsyncSocketIoRequest
    {
        AsyncSocketRecvIoRequest(const AsyncSocketRecvIoRequest&)            = delete;
        AsyncSocketRecvIoRequest(AsyncSocketRecvIoRequest&&)                 = delete;
        AsyncSocketRecvIoRequest& operator=(const AsyncSocketRecvIoRequest&) = delete;
        AsyncSocketRecvIoRequest& operator=(AsyncSocketRecvIoRequest&&)      = delete;

    public:
        /// <summary>The result flags of the completed receive.</summary>
        /// <returns>The <c>MSG_*</c> result flags (e.g. <c>MSG_PARTIAL</c>); <c>0</c> if none were reported.</returns>
        /// <remarks>Valid after completion only.</remarks>
        /// <seealso cref="SetFlags"/>
        DWORD GetFlags() const noexcept { return flags_; }

        /// <summary>Stores the result flags.</summary>
        /// <param name="flags">The <c>MSG_*</c> result flags of the receive.</param>
        /// <remarks><see cref="AsyncSocket"/> sets them when a receive completes synchronously.</remarks>
        /// <seealso cref="GetFlags"/>
        void SetFlags(DWORD flags) noexcept { flags_ = flags; }

        /// <summary>Records the result and, on success, fetches the result flags.</summary>
        /// <param name="error">The operation's own Win32/WSA error code; <c>ERROR_SUCCESS</c> (<c>0</c>) on success.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>The flags are read with <c>WSAGetOverlappedResult</c>.</remarks>
        void RequestCompleted(DWORD error, DWORD bytesTransferred) noexcept override
        {
            AsyncSocketIoRequest::RequestCompleted(error, bytesTransferred);

            if (Windows::WinsockError::Succeeded(error)) {
                auto& overlapped = GetOverlapped();
                WSAGetOverlappedResult(socket_, &overlapped, &bytesTransferred, false, &flags_);
            }
        }

    protected:
        /// <summary>Creates an idle request.</summary>
        /// <param name="socket">The socket the operation runs on; used to query the result flags.</param>
        explicit AsyncSocketRecvIoRequest(SOCKET socket) noexcept : socket_(socket) {}

    private:
        SOCKET socket_;
        DWORD  flags_ = 0;
    };

    /// <summary>Satisfied by <see cref="AsyncSocketIoRequest"/> and every class derived from it.</summary>
    /// <typeparam name="T">The type to test.</typeparam>
    export template <typename T>
    concept AsyncSocketIoRequestType = std::derived_from<T, AsyncSocketIoRequest>;

    /// <summary>A socket request a task can wait on until the operation completes.</summary>
    /// <typeparam name="_AsyncSocketIoRequest">
    /// The request built on: <see cref="AsyncSocketIoRequest"/>, or <see cref="AsyncSocketRecvIoRequest"/> to also
    /// capture the result flags of a receive.
    /// </typeparam>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of tasks may wait; <see cref="SuspensionKind::Single"/>: at
    /// most one waiting task, cheaper, allows a timeout.
    /// </typeparam>
    /// <remarks>
    /// A manual-reset event is set when the operation completes. Use the
    /// <see cref="WaitableAsyncSocketIoRequest"/>, <see cref="WaitableAsyncSocketIoRequest_s"/>,
    /// <see cref="WaitableAsyncSocketRecvIoRequest"/> and <see cref="WaitableAsyncSocketRecvIoRequest_s"/>
    /// variants below. The Single variant allows one waiting task and timed waits; once its
    /// <see cref="Wait"/> returns, nothing in the library touches the object again, so it may live on the
    /// waiting task's stack and be destroyed right away. The Multiple variant allows any number of waiting
    /// tasks but no timeout.
    /// </remarks>
    template <AsyncSocketIoRequestType _AsyncSocketIoRequest, SuspensionKind _SuspensionKind>
    class _WaitableAsyncSocketIoRequest : public _AsyncSocketIoRequest
    {
    public:
        /// <summary>Creates an idle request.</summary>
        /// <typeparam name="_Args">The types of the arguments passed on to the base request's constructor.</typeparam>
        /// <param name="args">
        /// Nothing for a plain request; for a receive request the <c>SOCKET</c> the operation runs on (used to
        /// query the result flags).
        /// </param>
        /// <remarks>A receive request takes the socket the operation runs on.</remarks>
        template <typename... _Args>
        explicit _WaitableAsyncSocketIoRequest(_Args&&... args) noexcept : _AsyncSocketIoRequest(std::forward<_Args>(args)...)
        {
            // forwarding instead of inheriting the constructors: inherited constructors would keep
            // the base's protected access
        }

        /// <summary>Records the result, then sets the event.</summary>
        /// <param name="error">The operation's own Win32/WSA error code; <c>ERROR_SUCCESS</c> (<c>0</c>) on success.</param>
        /// <param name="bytesTransferred">Bytes the operation transferred.</param>
        /// <remarks>Setting the event is the last access on behalf of the completion.</remarks>
        void RequestCompleted(DWORD error, DWORD bytesTransferred) noexcept override
        {
            _AsyncSocketIoRequest::RequestCompleted(error, bytesTransferred);
            event_.Set();
        }

        /// <summary>Re-arms the request for another operation.</summary>
        /// <remarks>Only after the previous operation has completed.</remarks>
        void Reset() noexcept override
        {
            _AsyncSocketIoRequest::Reset();
            event_.Reset();
        }

        /// <summary><c>true</c> if the operation has completed; never blocks.</summary>
        /// <returns><c>true</c> if the operation has completed; <c>false</c> while it is still in flight.</returns>
        /// <seealso cref="Wait"/>
        bool TryWait() noexcept { return event_.TryWait(); }

        /// <summary>Suspends the calling task until the operation has completed.</summary>
        /// <remarks>Task only.</remarks>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept { event_.Wait(); }

        /// <summary>Waits for the completion with a timeout.</summary>
        /// <param name="timeout">How long to wait at most.</param>
        /// <returns><c>true</c> if the operation has completed; <c>false</c> if the timeout expired first.</returns>
        /// <remarks>
        /// Single waiter only. Task only; needs <see cref="EnableTimers"/> for a finite timeout (without it
        /// the wait has no limit). A timeout does not cancel the operation: the request must stay alive until
        /// it has completed.
        /// </remarks>
        /// <seealso cref="AsyncSocket::CancelAsyncRequest"/>
        bool Wait(Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            return event_.Wait(timeout);
        }

    private:
        _ManualResetEvent<_SuspensionKind> event_;
    };

    /// <summary>Waitable socket request, any number of waiting tasks (Multiple), no timed wait.</summary>
    /// <seealso cref="WaitableAsyncSocketIoRequest_s"/>
    export using WaitableAsyncSocketIoRequest = _WaitableAsyncSocketIoRequest<AsyncSocketIoRequest, SuspensionKind::Multiple>;

    /// <summary>Waitable socket request, at most one waiting task (Single); cheaper, allows timed waits.</summary>
    /// <seealso cref="WaitableAsyncSocketIoRequest"/>
    export using WaitableAsyncSocketIoRequest_s = _WaitableAsyncSocketIoRequest<AsyncSocketIoRequest, SuspensionKind::Single>;

    /// <summary>Waitable receive request, any number of waiting tasks (Multiple).</summary>
    /// <remarks>Captures the result flags (see <see cref="AsyncSocketRecvIoRequest"/>).</remarks>
    /// <seealso cref="WaitableAsyncSocketRecvIoRequest_s"/>
    export using WaitableAsyncSocketRecvIoRequest = _WaitableAsyncSocketIoRequest<AsyncSocketRecvIoRequest, SuspensionKind::Multiple>;

    /// <summary>Waitable receive request, at most one waiting task (Single); allows timed waits.</summary>
    /// <remarks>Captures the result flags (see <see cref="AsyncSocketRecvIoRequest"/>).</remarks>
    /// <seealso cref="WaitableAsyncSocketRecvIoRequest"/>
    export using WaitableAsyncSocketRecvIoRequest_s = _WaitableAsyncSocketIoRequest<AsyncSocketRecvIoRequest, SuspensionKind::Single>;

    /// <summary>Satisfied by exactly the four waitable request types above.</summary>
    /// <typeparam name="T">
    /// The type to test: <see cref="WaitableAsyncSocketIoRequest"/>, <see cref="WaitableAsyncSocketIoRequest_s"/>,
    /// <see cref="WaitableAsyncSocketRecvIoRequest"/> or <see cref="WaitableAsyncSocketRecvIoRequest_s"/>.
    /// </typeparam>
    /// <remarks>Accepted by <see cref="AsyncSocket"/>'s waiting helpers.</remarks>
    export template <typename T>
    concept WaitableAsyncSocketIoRequestType = std::same_as<T, WaitableAsyncSocketIoRequest> || std::same_as<T, WaitableAsyncSocketIoRequest_s>
        || std::same_as<T, WaitableAsyncSocketRecvIoRequest> || std::same_as<T, WaitableAsyncSocketRecvIoRequest_s>;
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo