//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncSocket outside Windows: a socket whose operations run through the core's io_uring service
// (AsyncIo::SubmitIo). The same module as AsyncSocket.ixx, which holds the Windows class; CMake
// builds this file in its place, Visual Studio never sees it. It offers the part of the Windows
// class that TCP needs: accept, connect, receive and send into one buffer, message receive and
// send (msghdr), and the cancellation of operations. Recv, Send, CancelAsyncRequest and
// CancelAllAsyncRequests have the same forms as on Windows; AcceptAsync, Accept and the
// ConnectAsync with a sockaddr pointer differ (no AcceptEx/ConnectEx). Code for both platforms
// uses AsyncTcpSocket's Accept, Connect and ConnectAsync, which are the same on both.
//
// Windows only, not here: the WSABUF forms, receives with result flags, RecvFrom/SendTo,
// Disconnect, TransmitFile, TransmitPackets and GetOverlappedResult.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <utility>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocket;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
export import ThinkMeta.ConcurrentTasks.Posix.Socket;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocketIoRequest;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    /// <summary>A socket for asynchronous I/O from tasks.</summary>
    /// <remarks>
    /// Each operation is offered as a starting form (XxxAsync) and a waiting form (Xxx), as on Windows.
    /// Precondition: <see cref="EnableAsyncIo"/> must have succeeded. The socket needs no association:
    /// <see cref="Posix::Socket::Create"/> is all it takes.
    ///
    /// XxxAsync(..., asyncIoRequest) starts the operation and never suspends; it may be called from a
    /// task or a plain thread. The result is
    /// - <see cref="Posix::Error::Pending"/>: the operation is in flight; <see cref="AsyncSocketIoRequest::RequestCompleted"/>
    ///   is called later on the core's I/O thread with the operation's own <c>errno</c>. Unlike Windows,
    ///   no operation completes synchronously: also one that is done at once completes there.
    /// - any other code: the operation could not be started (e.g. <c>EAGAIN</c> when the kernel takes no
    ///   more for the moment); the request is not completed.
    /// The caller owns the request and every buffer, address and <c>msghdr</c> passed in; all of them must
    /// stay valid and must not move until the request has completed. A request is reused only after it
    /// has completed and been reset (<see cref="AsyncIoRequest::Reset"/>).
    ///
    /// Xxx(..., timeout) runs the same operation with a <see cref="WaitableAsyncSocketIoRequest_s"/> on
    /// its own stack and suspends the calling task until it has completed, so it must be called from a
    /// task. It returns the operation's final <c>errno</c>; *bytesTransferred is set to <c>0</c> on
    /// failure. A finite timeout needs <see cref="EnableTimers"/>. On expiry the operation is cancelled
    /// (<see cref="CancelIo"/>), the call still waits until the cancellation has completed, and
    /// <see cref="Posix::Error::TimedOut"/> is returned; if the operation completed before the
    /// cancellation took effect, its own result stands. A <see cref="CancelTask"/> of the waiting task
    /// ends the wait the same way and gives <see cref="Posix::Error::OperationAborted"/>
    /// (<c>ECANCELED</c>). After a cancelled operation a stream socket is fit for closing only.
    ///
    /// Sends never raise <c>SIGPIPE</c>: <c>MSG_NOSIGNAL</c> is always added, and a send to a closed
    /// connection fails with <c>EPIPE</c>. Several operations may be outstanding at once, each with its
    /// own request. <see cref="Close"/> and the destructor cancel what is still pending, as closing does
    /// on Windows; the requests must outlive those completions.
    /// </remarks>
    /// <seealso cref="Posix::Socket"/>
    /// <seealso cref="AsyncSocketIoRequest"/>
    export class AsyncSocket : public Posix::Socket
    {
        using SocketBase = Posix::Socket;
        using Error      = Posix::Error;

    public:
        /// <summary>The error code of every operation: an <c>errno</c>, <c>0</c> on success.</summary>
        using ErrorCode = Posix::ErrorCode;

        /// <summary>A byte count of one operation.</summary>
        using ByteCount = AsyncIoRequestBase::ByteCount;

        /// <summary>Creates an object without a socket.</summary>
        AsyncSocket() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; no operation may be pending on it.</param>
        AsyncSocket(AsyncSocket&& other) noexcept = default;

        /// <summary>Cancels the pending operations and closes the socket (see <see cref="Close"/>).</summary>
        ~AsyncSocket() noexcept { Close(); }

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; no operation may be pending on it.</param>
        /// <returns>This object.</returns>
        /// <remarks>A socket this object already holds is closed first (see <see cref="Close"/>).</remarks>
        AsyncSocket& operator=(AsyncSocket&& other) noexcept
        {
            if (this != &other) {
                Close();
                SocketBase::operator=(std::move(other));
            }
            return *this;
        }

        /// <summary>Cancels every pending operation, then closes the socket, if there is one.</summary>
        /// <returns><see cref="Posix::Error::Success"/>, or the <c>errno</c> of <c>close</c>.</returns>
        /// <remarks>
        /// As <c>closesocket</c> on Windows: the pending operations end, normally with
        /// <see cref="Posix::Error::OperationAborted"/>; their requests and buffers must stay valid until
        /// those completions have been delivered. A plain <c>close</c> would not end them on Linux. If the
        /// kernel refuses the cancel (<c>EAGAIN</c>, <c>EBUSY</c>) or cannot cancel by file (before Linux
        /// 5.19), the socket is shut down instead: a pending receive then ends with <c>0</c> bytes, an
        /// accept with <c>EINVAL</c>, a send with <c>EPIPE</c> - ended in any case, if not with the same
        /// code. Costs one system call more than a plain close.
        /// </remarks>
        /// <seealso cref="CancelAllAsyncRequests"/>
        ErrorCode Close() noexcept
        {
            // before the close: the kernel looks the socket up by its number
            if (IsValid() && !CancelAllIo(GetSocket()))
                SocketBase::Shutdown(SHUT_RDWR);
            return SocketBase::Close();
        }

        /// <summary>Requests cancellation of every pending operation on this socket (<see cref="CancelAllIo"/>).</summary>
        /// <returns>
        /// <see cref="Posix::Error::Success"/> if the cancellation was passed on; <c>EOPNOTSUPP</c> before Linux 5.19;
        /// or the <c>errno</c> of the submission (e.g. <c>EAGAIN</c>).
        /// </returns>
        /// <remarks>
        /// Covers operations started from any task or thread before the call. Does not wait: each cancelled
        /// operation still completes (normally with <see cref="Posix::Error::OperationAborted"/>); keep its
        /// request and buffers alive until then. Unlike Windows, which reports <c>ERROR_NOT_FOUND</c>, success
        /// does not tell whether any operation was pending.
        /// </remarks>
        /// <seealso cref="CancelAsyncRequest"/>
        ErrorCode CancelAllAsyncRequests() noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(CancelAllIo(GetSocket()));
        }

        /// <summary>Starts an accept on this listening socket (<c>accept4</c>).</summary>
        /// <param name="address">Receives the peer's address; <c>nullptr</c> if not wanted.</param>
        /// <param name="addressLength">
        /// In: the size of <paramref name="address"/>; out: the size of the peer's address. <c>nullptr</c>
        /// together with <paramref name="address"/>.
        /// </param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <remarks>
        /// On success the request's <see cref="AsyncIoRequest::GetTransferredBytes"/> is the new socket's
        /// descriptor, created with <c>SOCK_CLOEXEC</c>: take it over with <see cref="Posix::Socket::Attach"/>,
        /// or it leaks. The waiting form <see cref="Accept"/> does that.
        /// </remarks>
        /// <seealso cref="Posix::Socket::Listen"/>
        ErrorCode AcceptAsync(void* address, socklen_t* addressLength, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            // an address without its length would fail after the accept, and the connection be lost
            CT_ASSERT((address == nullptr) == (addressLength == nullptr));

            return Submit(asyncIoRequest, IoOperation::Accept, address, 0, reinterpret_cast<std::uintptr_t>(addressLength), SOCK_CLOEXEC);
        }

        /// <summary>Accepts a connection into <paramref name="acceptSocket"/>.</summary>
        /// <param name="acceptSocket">Takes the new connection; must not hold a socket.</param>
        /// <param name="address">Receives the peer's address; <c>nullptr</c> if not wanted.</param>
        /// <param name="addressLength">In: the size of <paramref name="address"/>; out: the size of the peer's address.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until a connection has arrived.</remarks>
        ErrorCode Accept(Socket& acceptSocket, void* address = nullptr, socklen_t* addressLength = nullptr, Duration timeout = Duration::Infinite()) noexcept
        {
            CT_ASSERT(!acceptSocket.IsValid());

            WaitableAsyncSocketIoRequest_s asyncIoRequest;
            ByteCount                      accepted = 0; // the new descriptor

            auto error = AcceptAsync(address, addressLength, asyncIoRequest);
            error      = WaitForAsyncIoOperation(error, &accepted, asyncIoRequest, timeout);
            if (Error::Succeeded(error))
                acceptSocket.Attach(static_cast<int>(accepted));
            return error;
        }

        /// <summary>Starts a connect (<c>connect</c>).</summary>
        /// <param name="address">The remote address; must stay valid until the operation has completed.</param>
        /// <param name="addressLength">The size of <paramref name="address"/> in bytes.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <remarks>Needs no <see cref="Posix::Socket::Bind"/> before, unlike <c>ConnectEx</c> on Windows.</remarks>
        ErrorCode ConnectAsync(const sockaddr* address, socklen_t addressLength, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return Submit(asyncIoRequest, IoOperation::Connect, const_cast<sockaddr*>(address), addressLength, 0, 0);
        }

        /// <summary>Starts a connect to an address of type <typeparamref name="_SockAddr"/>.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="address">The remote address; must stay valid until the operation has completed.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        template <Posix::SockAddrType _SockAddr>
        ErrorCode ConnectAsync(const _SockAddr& address, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return ConnectAsync(reinterpret_cast<const sockaddr*>(&address), sizeof(address), asyncIoRequest);
        }

        /// <summary>Connects to <paramref name="address"/>.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="address">The remote address.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>
        /// (e.g. <c>ECONNREFUSED</c>).
        /// </returns>
        /// <remarks>Task only; suspends the calling task. Hides the blocking <see cref="Posix::Socket::Connect"/>.</remarks>
        template <Posix::SockAddrType _SockAddr>
        ErrorCode Connect(const _SockAddr& address, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = ConnectAsync(address, asyncIoRequest);
            return WaitForAsyncIoOperation(error, nullptr, asyncIoRequest, timeout);
        }

        /// <summary>Requests cancellation of one operation (<see cref="CancelIo"/>).</summary>
        /// <param name="asyncIoRequest">The request the operation was started with.</param>
        /// <returns><see cref="Posix::Error::Success"/> if the cancellation was passed on, or its <c>errno</c>.</returns>
        /// <remarks>
        /// Does not wait: the request still completes, normally with <see cref="Posix::Error::OperationAborted"/>,
        /// or with its own result if it finished first. Keep the request and its buffers alive until then.
        /// After a cancelled operation a stream socket is fit for closing only.
        /// </remarks>
        ErrorCode CancelAsyncRequest(AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(CancelIo(&asyncIoRequest.GetIoRequest()));
        }

        /// <summary>Starts a receive into one buffer (<c>recv</c>).</summary>
        /// <param name="buffer">The receive buffer; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Not written: no operation completes synchronously here. There for the same call as on Windows; may be <c>nullptr</c>.
        /// </param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <remarks>
        /// On a stream socket, a completion with <see cref="Posix::Error::Success"/> and <c>0</c> bytes means
        /// the peer closed the connection.
        /// </remarks>
        /// <seealso cref="Recv"/>
        ErrorCode RecvAsync(void* buffer, std::uint32_t bufferSize, [[maybe_unused]] ByteCount* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return Submit(asyncIoRequest, IoOperation::Recv, buffer, bufferSize, 0, 0);
        }

        /// <summary>Receives into one buffer.</summary>
        /// <param name="buffer">The receive buffer.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. <see cref="Posix::Error::Success"/> with <c>0</c> bytes on a
        /// stream socket: the peer closed the connection.
        /// </remarks>
        ErrorCode Recv(void* buffer, std::uint32_t bufferSize, ByteCount* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = RecvAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a send of one buffer (<c>send</c>).</summary>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">Not written (see <see cref="RecvAsync"/>); may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags (e.g. <c>MSG_MORE</c>); <c>MSG_NOSIGNAL</c> is always added.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <remarks>A stream socket may send fewer bytes than asked: check the transferred bytes.</remarks>
        /// <seealso cref="Send"/>
        ErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, [[maybe_unused]] ByteCount* bytesTransferred, std::uint32_t flags,
                            AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return Submit(asyncIoRequest, IoOperation::Send, const_cast<void*>(buffer), bufferSize, 0, flags | MSG_NOSIGNAL);
        }

        /// <summary>Starts a send of one buffer without flags.</summary>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">Not written (see <see cref="RecvAsync"/>); may be <c>nullptr</c>.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        ErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, ByteCount* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return SendAsync(buffer, bufferSize, bytesTransferred, 0, asyncIoRequest);
        }

        /// <summary>Sends one buffer.</summary>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags; <c>MSG_NOSIGNAL</c> is always added.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        ErrorCode Send(const void* buffer, std::uint32_t bufferSize, ByteCount* bytesTransferred, std::uint32_t flags,
                       Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Sends one buffer without flags.</summary>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>As the form with flags.</returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        ErrorCode Send(const void* buffer, std::uint32_t bufferSize, ByteCount* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            return Send(buffer, bufferSize, bytesTransferred, 0, timeout);
        }

        /// <summary>Starts a message receive (<c>recvmsg</c>).</summary>
        /// <param name="msg">
        /// The message: buffers (<c>msg_iov</c>), optional address and control data. It and everything it points
        /// to must stay valid until the operation has completed; the result flags land in <c>msg_flags</c>.
        /// </param>
        /// <param name="bytesTransferred">Not written (see <see cref="RecvAsync"/>); may be <c>nullptr</c>.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <seealso cref="RecvMsg"/>
        ErrorCode RecvMsgAsync(msghdr* msg, [[maybe_unused]] ByteCount* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return Submit(asyncIoRequest, IoOperation::RecvMsg, msg, 0, 0, 0);
        }

        /// <summary>Receives a message.</summary>
        /// <param name="msg">The message (see <see cref="RecvMsgAsync"/>).</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        ErrorCode RecvMsg(msghdr* msg, ByteCount* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = RecvMsgAsync(msg, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a message send (<c>sendmsg</c>).</summary>
        /// <param name="msg">
        /// The message: buffers (<c>msg_iov</c>), optional address and control data. It and everything it points
        /// to must stay valid until the operation has completed.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags; <c>MSG_NOSIGNAL</c> is always added.</param>
        /// <param name="bytesTransferred">Not written (see <see cref="RecvAsync"/>); may be <c>nullptr</c>.</param>
        /// <param name="asyncIoRequest">The caller-owned request (see the class remarks).</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of the start.</returns>
        /// <seealso cref="SendMsg"/>
        ErrorCode SendMsgAsync(msghdr* msg, std::uint32_t flags, [[maybe_unused]] ByteCount* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return Submit(asyncIoRequest, IoOperation::SendMsg, msg, 0, 0, flags | MSG_NOSIGNAL);
        }

        /// <summary>Sends a message.</summary>
        /// <param name="msg">The message (see <see cref="SendMsgAsync"/>).</param>
        /// <param name="flags"><c>MSG_*</c> flags; <c>MSG_NOSIGNAL</c> is always added.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="Posix::Error::Success"/>, <see cref="Posix::Error::TimedOut"/>,
        /// <see cref="Posix::Error::OperationAborted"/> if the task was cancelled, or the operation's <c>errno</c>.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        ErrorCode SendMsg(msghdr* msg, std::uint32_t flags, ByteCount* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendMsgAsync(msg, flags, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

    protected:
        /// <summary>Fills the request's <see cref="IoRequest"/> and hands it to the core.</summary>
        /// <param name="asyncIoRequest">The request; idle (new, or completed and reset).</param>
        /// <param name="operation">The operation.</param>
        /// <param name="buffer">The data, address or <c>msghdr</c> (see <see cref="IoOperation"/>).</param>
        /// <param name="length">The number of bytes, or the address length of a connect.</param>
        /// <param name="offset">The <c>socklen_t*</c> of an accept; else <c>0</c>.</param>
        /// <param name="flags">The <c>MSG_*</c> or <c>SOCK_*</c> flags.</param>
        /// <returns><see cref="Posix::Error::Pending"/>, or the <c>errno</c> of <see cref="SubmitIo"/>.</returns>
        ErrorCode Submit(AsyncSocketIoRequest& asyncIoRequest, IoOperation operation, void* buffer, std::uint32_t length, std::uint64_t offset,
                         std::uint32_t flags) noexcept
        {
            CT_ASSERT(IsValid());

            auto& request     = asyncIoRequest.GetIoRequest(); // auto: IoRequest names the private base here
            request.routine   = &AsyncIoRequestCompleted;
            request.buffer    = buffer;
            request.offset    = offset;
            request.fd        = GetSocket();
            request.length    = length;
            request.flags     = flags;
            request.operation = operation;

            // every accepted operation completes on the I/O thread, also one that is done at once
            return SubmitIo(&request) ? Error::Pending : Error::GetLastError();
        }

        /// <summary>Waits for a started operation, cancelling it on a timeout or a cancel of the task.</summary>
        /// <typeparam name="_Request">The single-waiter request type.</typeparam>
        /// <param name="error">What the start returned; anything but Pending is returned as it is.</param>
        /// <param name="bytesTransferred">Receives the transferred bytes (<c>0</c> on failure); may be <c>nullptr</c>.</param>
        /// <param name="asyncIoRequest">The request the operation was started with.</param>
        /// <param name="timeout">Maximum time to wait.</param>
        /// <returns>The operation's final <c>errno</c>, or <see cref="Posix::Error::TimedOut"/>.</returns>
        template <WaitableAsyncSocketIoRequestType _Request>
        ErrorCode WaitForAsyncIoOperation(ErrorCode error, ByteCount* bytesTransferred, _Request& asyncIoRequest,
                                          Duration timeout = Duration::Infinite()) noexcept
        {
            if (Error::IsPending(error)) {
                // ends on the timeout or on a cancel of the task, also without a limit
                if (const auto status = asyncIoRequest.Wait(timeout); status != WaitStatus::Signaled) {
                    // Timed out or cancelled: cancel the operation, then wait for its completion without
                    // a limit - the request lives in this frame. If it completed meanwhile, its result
                    // stands. A cancelled socket operation leaves the socket fit for closing only.
                    _CancelIoRetrying(&asyncIoRequest.GetIoRequest());
                    asyncIoRequest.Wait();
                    error = _ErrorAfterCancel<Error>(asyncIoRequest.GetError(), status);
                } else {
                    error = asyncIoRequest.GetError();
                }
            }

            if (bytesTransferred)
                *bytesTransferred = Error::Succeeded(error) ? asyncIoRequest.GetTransferredBytes() : 0;

            return error;
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo
