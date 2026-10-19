//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncSocket: a Winsock socket whose overlapped operations complete through the core's I/O
// completion port (AsyncIo::EnableAsyncIo). Every operation comes as a non-suspending *Async
// form that takes a caller-owned AsyncSocketIoRequest and as a form that suspends the calling
// task until the operation has completed, optionally with a timeout.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>

#include <Windows.h>

#include <MSWSock.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocket;

import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIoRequest;
export import ThinkMeta.ConcurrentTasks.Windows.Socket;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocketIoRequest;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    /// <summary>A socket for overlapped I/O from tasks.</summary>
    /// <remarks>
    /// <see cref="Create"/> associates it with the core's completion port, and each operation is offered
    /// as a starting form (XxxAsync) and a waiting form (Xxx).
    ///
    /// Preconditions: <see cref="Windows::Socket::Startup"/> (<c>WSAStartup</c> plus the
    /// <c>AcceptEx</c>/<c>ConnectEx</c>/... extension pointers) and <see cref="EnableAsyncIo"/> must have
    /// succeeded before Create, and the socket must be created through this class's Create (directly or
    /// through <see cref="AsyncTcpSocket"/>/<see cref="AsyncIpSocket"/>), otherwise no completion ever
    /// arrives.
    ///
    /// XxxAsync(..., asyncIoRequest) starts the operation and never suspends; it may be called from a
    /// task or a plain thread. The result is
    /// - <see cref="Windows::WinsockError::Success"/>: the operation completed synchronously. The request
    ///   has already been completed (<see cref="AsyncSocketIoRequest::RequestCompleted"/> ran on the calling
    ///   thread before the return); the core sets <c>FILE_SKIP_COMPLETION_PORT_ON_SUCCESS</c> on the
    ///   handle, so no completion packet follows.
    /// - <see cref="Windows::WinsockError::Pending"/>: the operation is in flight; RequestCompleted is
    ///   called later on the core's I/O thread with the operation's own error code.
    /// - any other WSA code: the operation failed synchronously; the request is not completed.
    /// The caller owns the request (<see cref="AsyncSocketIoRequest"/>) and every buffer (and
    /// address/<c>WSAMSG</c> structure) passed in; all of them must stay valid and must not move until the
    /// request has completed. A request is reused only after it has completed and been reset
    /// (<see cref="AsyncIoRequest::Reset"/>).
    ///
    /// Xxx(..., timeout) runs the same operation with a <see cref="WaitableAsyncSocketIoRequest_s"/> on
    /// its own stack and suspends the calling task until it has completed (the thread runs other tasks
    /// meanwhile), so it must be called from a task. It returns the operation's final WSA code;
    /// *bytesTransferred is set to <c>0</c> on failure. A finite timeout needs <see cref="EnableTimers"/>
    /// (without it the wait has no limit). On expiry the operation is cancelled (<c>CancelIoEx</c>), the
    /// call still waits until the cancellation has completed, and <see cref="Windows::WinsockError::TimedOut"/>
    /// is returned; if the operation completed before the cancellation took effect, its own result stands.
    /// After a cancelled operation a stream socket is fit for closing only (data may have been lost).
    ///
    /// Errors are reported as <see cref="Windows::WinsockErrorCode"/> (WSA* values); nothing throws.
    /// Several operations may be outstanding at once (e.g. one receive and one send), each with its own
    /// request, started from any tasks or threads. Close or destroy the socket only when no operation is
    /// pending any more (cancel with <see cref="CancelAsyncRequest"/>/<see cref="Windows::Socket::CancelAllAsyncRequests"/>
    /// and wait for the completions first).
    /// </remarks>
    /// <seealso cref="Windows::Socket"/>
    /// <seealso cref="AsyncSocketIoRequest"/>
    export class AsyncSocket : public Windows::Socket
    {
        using SocketBase       = Windows::Socket;
        using WinsockError     = Windows::WinsockError;
        using WinsockErrorCode = Windows::WinsockErrorCode;

    public:
        /// <summary>Creates the socket and associates it with the core's completion port.</summary>
        /// <param name="addressFamily">The address family, e.g. <c>AF_INET</c> or <c>AF_INET6</c>.</param>
        /// <param name="type">The socket type, e.g. <c>SOCK_STREAM</c> or <c>SOCK_DGRAM</c>.</param>
        /// <param name="protocol">
        /// The protocol, e.g. <c>IPPROTO_TCP</c> or <c>IPPROTO_UDP</c>; <c>0</c> lets the provider choose for the family and type.
        /// </param>
        /// <param name="protocolInfo">
        /// Optional <c>WSAPROTOCOL_INFO</c> selecting the provider (as for <c>WSASocket</c>); <c>nullptr</c> (the default) uses
        /// <paramref name="addressFamily"/>, <paramref name="type"/> and <paramref name="protocol"/>. Only read during the call.
        /// </param>
        /// <param name="g">The Winsock socket group; <c>0</c> (the default) for none.</param>
        /// <param name="flags">
        /// Further <c>WSA_FLAG_*</c> flags for <c>WSASocket</c>, e.g. <c>WSA_FLAG_NO_HANDLE_INHERIT</c>; <c>0</c> (the default)
        /// for none. <c>WSA_FLAG_OVERLAPPED</c> is always added.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, the WSA error of <c>WSASocket</c>, or the error of the
        /// association with the completion port (then the new socket has been closed again and the object
        /// holds none).
        /// </returns>
        /// <remarks>
        /// Parameters as <c>WSASocket</c>; <c>WSA_FLAG_OVERLAPPED</c> is added to <paramref name="flags"/>.
        /// The object must not hold a socket yet. Needs <see cref="Windows::Socket::Startup"/> and
        /// <see cref="EnableAsyncIo"/>.
        /// </remarks>
        /// <seealso cref="Windows::Socket::Close"/>
        WinsockErrorCode Create(int addressFamily, int type, int protocol, WSAPROTOCOL_INFO* protocolInfo = nullptr, GROUP g = 0, DWORD flags = 0) noexcept
        {
            CT_ASSERT(!IsValid());

            auto error = SocketBase::Create(addressFamily, type, protocol, protocolInfo, g, flags | WSA_FLAG_OVERLAPPED);
            // A socket that cannot be associated would never see its completions: close it again.
            if (IsValid() && !AssociateHandleWithIoCompletionPort(reinterpret_cast<HANDLE>(GetSocket()), &AsyncIoRequestCompleted)) {
                error = WinsockErrorCode(::GetLastError());
                Close();
                if (WinsockError::Succeeded(error))
                    error = WinsockError::InvalidHandle;
            }

            return error;
        }

        // Keep the base class's raw OVERLAPPED overloads of each operation visible next to the
        // request-based ones below.
        using SocketBase::AcceptAsync;

        /// <summary>Starts an <c>AcceptEx</c> on this listening socket.</summary>
        /// <param name="acceptSocket">Takes the accepted connection. A created, unbound and unconnected socket.
        /// For later asynchronous I/O it must be an AsyncSocket created through <see cref="Create"/>.</param>
        /// <param name="outputBuffer">Receives the first <paramref name="receiveDataLength"/> bytes of data followed by the
        /// local and remote address (parse with <c>GetAcceptExSockaddrs</c>). Must stay valid until completion.</param>
        /// <param name="receiveDataLength">Bytes of initial data to wait for; <c>0</c> completes as soon as a
        /// connection arrives, otherwise only after that data (or the connection's end).</param>
        /// <param name="localAddressLength">Space reserved for the local address: at least the address size + 16.</param>
        /// <param name="remoteAddressLength">Space reserved for the remote address: at least the address size + 16.</param>
        /// <param name="bytesReceived">Required. Receives the byte count on synchronous completion.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// After the accept has completed, call acceptSocket.<see cref="Windows::Socket::SetOptionUpdateAcceptContext"/>(*this)
        /// before <c>getpeername</c>/<c>shutdown</c> etc. are used on it; <see cref="AsyncTcpSocket::Accept"/>
        /// does that.
        /// </remarks>
        /// <seealso cref="Accept"/>
        /// <seealso cref="AsyncSocketIoRequest"/>
        WinsockErrorCode AcceptAsync(Socket& acceptSocket, void* outputBuffer, DWORD receiveDataLength, DWORD localAddressLength, DWORD remoteAddressLength,
                                     DWORD* bytesReceived, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            CT_ASSERT(bytesReceived);

            auto error = SocketBase::AcceptAsync(acceptSocket, outputBuffer, receiveDataLength, localAddressLength, remoteAddressLength, bytesReceived,
                                                 asyncIoRequest.GetOverlapped());
            return HandleCompletedRequest(error, bytesReceived, asyncIoRequest);
        }

        /// <summary>Accepts a connection on this listening socket (<c>AcceptEx</c>).</summary>
        /// <param name="acceptSocket">Takes the accepted connection. A created, unbound and unconnected socket.
        /// For later asynchronous I/O it must be an AsyncSocket created through <see cref="Create"/>.</param>
        /// <param name="outputBuffer">Receives the first <paramref name="receiveDataLength"/> bytes of data followed by the
        /// local and remote address (parse with <c>GetAcceptExSockaddrs</c>).</param>
        /// <param name="receiveDataLength">Bytes of initial data to wait for; <c>0</c> completes as soon as a
        /// connection arrives, otherwise only after that data (or the connection's end).</param>
        /// <param name="localAddressLength">Space reserved for the local address: at least the address size + 16.</param>
        /// <param name="remoteAddressLength">Space reserved for the remote address: at least the address size + 16.</param>
        /// <param name="bytesReceived">
        /// Required. Receives the bytes of initial data received once the accept has completed (<c>0</c> on failure).
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task until a connection has been accepted into
        /// <paramref name="acceptSocket"/>. Does not set
        /// <c>SO_UPDATE_ACCEPT_CONTEXT</c>; call acceptSocket.<see cref="Windows::Socket::SetOptionUpdateAcceptContext"/>(*this)
        /// afterwards (or use <see cref="AsyncTcpSocket::Accept"/>).
        /// </remarks>
        /// <seealso cref="AcceptAsync"/>
        WinsockErrorCode Accept(Socket& acceptSocket, void* outputBuffer, DWORD receiveDataLength, DWORD localAddressLength, DWORD remoteAddressLength,
                                DWORD* bytesReceived, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = AcceptAsync(acceptSocket, outputBuffer, receiveDataLength, localAddressLength, remoteAddressLength, bytesReceived, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesReceived, asyncIoRequest, timeout);
        }

        using SocketBase::ConnectAsync;

        /// <summary>Starts a <c>ConnectEx</c>.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="addr">Required. The address to connect to.</param>
        /// <param name="addrSize">The size of the actual address at <paramref name="addr"/> (e.g. <c>sizeof(sockaddr_in)</c> for an
        /// IPv4 address held in a <c>sockaddr_storage</c>).</param>
        /// <param name="sendBuffer">
        /// Optional data sent after the connection is established; may be <c>nullptr</c>. Must stay valid until the operation
        /// has completed.
        /// </param>
        /// <param name="sendDataLength">Bytes to send from <paramref name="sendBuffer"/>; <c>0</c> without it.</param>
        /// <param name="bytesSent">Required when <paramref name="sendBuffer"/> is given; receives the bytes sent on synchronous completion.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// Connection-oriented sockets only. <c>ConnectEx</c> needs a bound socket:
        /// <see cref="Windows::Socket::Bind"/> (e.g. to the any address, port <c>0</c>) first, otherwise it
        /// fails with <c>WSAEINVAL</c>. After the connect has completed, call
        /// <see cref="Windows::Socket::SetOptionUpdateConnectContext"/> (<see cref="AsyncTcpSocket::Connect"/>
        /// does that).
        /// </remarks>
        /// <seealso cref="Connect"/>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode ConnectAsync(const _SockAddr* addr, int addrSize, void* sendBuffer, DWORD sendDataLength, DWORD* bytesSent,
                                      AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            auto  error = SocketBase::ConnectAsync(reinterpret_cast<const sockaddr*>(addr), addrSize, sendBuffer, sendDataLength, bytesSent,
                                                   asyncIoRequest.GetOverlapped());
            DWORD none  = 0; // without a count nothing was sent; no need to ask the operation
            return HandleCompletedRequest(error, bytesSent ? bytesSent : &none, asyncIoRequest);
        }

        /// <summary>Connects with <c>ConnectEx</c>.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="addr">Required. The address to connect to.</param>
        /// <param name="addrSize">The size of the actual address at <paramref name="addr"/> (e.g. <c>sizeof(sockaddr_in)</c> for an
        /// IPv4 address held in a <c>sockaddr_storage</c>).</param>
        /// <param name="sendBuffer">Optional data sent after the connection is established; may be <c>nullptr</c>.</param>
        /// <param name="sendDataLength">Bytes to send from <paramref name="sendBuffer"/>; <c>0</c> without it.</param>
        /// <param name="bytesSent">
        /// Required when <paramref name="sendBuffer"/> is given; receives the bytes sent once connected (<c>0</c> on failure).
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task until connected.
        /// The socket must be bound. Does not set <c>SO_UPDATE_CONNECT_CONTEXT</c>; call
        /// <see cref="Windows::Socket::SetOptionUpdateConnectContext"/> afterwards (or use
        /// <see cref="AsyncTcpSocket::Connect"/>).
        /// </remarks>
        /// <seealso cref="ConnectAsync"/>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode Connect(const _SockAddr* addr, int addrSize, void* sendBuffer, DWORD sendDataLength, DWORD* bytesSent,
                                 Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = ConnectAsync(addr, addrSize, sendBuffer, sendDataLength, bytesSent, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesSent, asyncIoRequest, timeout);
        }

        using SocketBase::DisconnectAsync;

        /// <summary>Starts a <c>DisconnectEx</c> on a connected socket.</summary>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <param name="flags">
        /// <c>0</c> or <c>TF_REUSE_SOCKET</c> (the socket can then be passed to <see cref="AcceptAsync"/>/
        /// <see cref="ConnectAsync"/> again).
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <seealso cref="Disconnect"/>
        WinsockErrorCode DisconnectAsync(AsyncSocketIoRequest& asyncIoRequest, DWORD flags = 0) noexcept
        {
            auto  error = SocketBase::DisconnectAsync(asyncIoRequest.GetOverlapped(), flags);
            DWORD none  = 0; // a disconnect transfers nothing
            return HandleCompletedRequest(error, &none, asyncIoRequest);
        }

        /// <summary>Disconnects with <c>DisconnectEx</c>.</summary>
        /// <param name="flags">
        /// <c>0</c> (the default) or <c>TF_REUSE_SOCKET</c> (the socket can then be passed to <see cref="Accept"/>/
        /// <see cref="Connect"/> again).
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        /// <seealso cref="DisconnectAsync"/>
        WinsockErrorCode Disconnect(DWORD flags = 0, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = DisconnectAsync(asyncIoRequest, flags);
            return WaitForAsyncIoOperation(error, nullptr, asyncIoRequest, timeout);
        }

        /// <summary>Reads the result of an operation without waiting.</summary>
        /// <param name="asyncIoRequest">The request the operation was started with.</param>
        /// <param name="bytesTransferred">Required. Receives the bytes transferred.</param>
        /// <param name="flags">Receives the operation's result flags.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <c>WSA_IO_INCOMPLETE</c> while still pending, or the
        /// operation's error.
        /// </returns>
        /// <remarks><c>WSAGetOverlappedResult</c> with <c>fWait</c> = <c>false</c>.</remarks>
        WinsockErrorCode GetOverlappedResult(AsyncSocketIoRequest& asyncIoRequest, DWORD* bytesTransferred, DWORD* flags) noexcept
        {
            return SocketBase::GetOverlappedResult(asyncIoRequest.GetOverlapped(), bytesTransferred, false, flags);
        }

        /// <summary>Requests cancellation of one operation (<c>CancelIoEx</c>).</summary>
        /// <param name="asyncIoRequest">The request the operation was started with.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> if cancellation was requested, <c>ERROR_NOT_FOUND</c> if the
        /// operation was no longer pending, or another error.
        /// </returns>
        /// <remarks>
        /// Does not wait: the request still completes, normally with <see cref="WinsockError::OperationAborted"/>,
        /// or with its own result if it finished first. Keep the request and its buffers alive until then.
        /// After a cancelled operation a stream socket is fit for closing only.
        /// </remarks>
        /// <seealso cref="Windows::Socket::CancelAllAsyncRequests"/>
        WinsockErrorCode CancelAsyncRequest(AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetAsyncError(CancelIoEx(reinterpret_cast<HANDLE>(GetSocket()), &asyncIoRequest.GetOverlapped()));
        }

        using SocketBase::RecvAsync;

        /// <summary>Starts a <c>WSARecv</c>.</summary>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call. The result flags (e.g. <c>MSG_PARTIAL</c>) are stored in
        /// <paramref name="asyncIoRequest"/> (<see cref="AsyncSocketRecvIoRequest::GetFlags"/>) once the operation has completed.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned receive request that is completed with the operation's result and keeps its result flags. It must
        /// not be in use by another operation and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// On a stream socket, a completion with <see cref="WinsockError::Success"/> and <c>0</c> bytes means
        /// the peer closed the connection.
        /// </remarks>
        /// <seealso cref="Recv"/>
        /// <seealso cref="AsyncSocketRecvIoRequest"/>
        WinsockErrorCode RecvAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecv</c>.</summary>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call; out on success: the result flags (e.g. <c>MSG_PARTIAL</c>).</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. <see cref="WinsockError::Success"/> with <c>0</c> bytes on a
        /// stream socket: the peer closed the connection.
        /// </remarks>
        /// <seealso cref="RecvAsync"/>
        WinsockErrorCode Recv(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketRecvIoRequest_s asyncIoRequest(GetSocket());

            auto error = RecvAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest);
            error = WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
            if (WinsockError::Succeeded(error))
                *flags = asyncIoRequest.GetFlags();
            return error;
        }

        /// <summary>Starts a <c>WSARecv</c> without flags.</summary>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>See the overload with flags.</remarks>
        WinsockErrorCode RecvAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecv</c> without flags.</summary>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. <see cref="WinsockError::Success"/> with <c>0</c> bytes on a
        /// stream socket: the peer closed the connection.
        /// </remarks>
        WinsockErrorCode Recv(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = RecvAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSARecv</c> into one buffer.</summary>
        /// <param name="buffer">The receive buffer; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call. The result flags (e.g. <c>MSG_PARTIAL</c>) are stored in
        /// <paramref name="asyncIoRequest"/> (<see cref="AsyncSocketRecvIoRequest::GetFlags"/>) once the operation has completed.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned receive request that is completed with the operation's result and keeps its result flags. It must
        /// not be in use by another operation and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>See the <c>WSABUF</c> overload.</remarks>
        WinsockErrorCode RecvAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags,
                                   AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecv</c>.</summary>
        /// <param name="buffer">The receive buffer.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call; out on success: the result flags (e.g. <c>MSG_PARTIAL</c>).</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. <see cref="WinsockError::Success"/> with <c>0</c> bytes on a
        /// stream socket: the peer closed the connection.
        /// </remarks>
        WinsockErrorCode Recv(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketRecvIoRequest_s asyncIoRequest(GetSocket());

            auto error = RecvAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest);
            error = WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
            if (WinsockError::Succeeded(error))
                *flags = asyncIoRequest.GetFlags();
            return error;
        }

        /// <summary>Starts a <c>WSARecv</c> into one buffer without flags.</summary>
        /// <param name="buffer">The receive buffer; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>See the <c>WSABUF</c> overload.</remarks>
        WinsockErrorCode RecvAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecv</c> without flags.</summary>
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
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. <see cref="WinsockError::Success"/> with <c>0</c> bytes on a
        /// stream socket: the peer closed the connection. With a timeout, a receive that times out is
        /// cancelled and returns <see cref="WinsockError::TimedOut"/>.
        /// </remarks>
        WinsockErrorCode Recv(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = RecvAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        using SocketBase::SendAsync;

        /// <summary>Starts a <c>WSASend</c>.</summary>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <seealso cref="Send"/>
        WinsockErrorCode SendAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends with <c>WSASend</c>.</summary>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        /// <seealso cref="SendAsync"/>
        WinsockErrorCode Send(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASend</c> without flags.</summary>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        WinsockErrorCode SendAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends with <c>WSASend</c> without flags.</summary>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        WinsockErrorCode Send(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASend</c> of one buffer.</summary>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>The buffer must stay valid until completion.</remarks>
        WinsockErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends one buffer with <c>WSASend</c>.</summary>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        WinsockErrorCode Send(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags,
                              Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASend</c> of one buffer without flags.</summary>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>The buffer must stay valid until completion.</remarks>
        WinsockErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends one buffer with <c>WSASend</c> without flags.</summary>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        WinsockErrorCode Send(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        using SocketBase::RecvFromAsync;

        /// <summary>Starts a <c>WSARecvFrom</c> without capturing the sender's address.</summary>
        /// <remarks>Without an address the same as <see cref="RecvAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="RecvAsync"/>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags,
                                       AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return RecvAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecvFrom</c>.</summary>
        /// <remarks>Without an address the same as <see cref="Recv"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Recv"/>
        WinsockErrorCode RecvFrom(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, Duration timeout = Duration::Infinite()) noexcept
        {
            return Recv(buffers, bufferCount, bytesTransferred, flags, timeout);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> without flags or sender address.</summary>
        /// <remarks>Without an address the same as <see cref="RecvAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="RecvAsync"/>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return RecvAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecvFrom</c> without flags or sender address.</summary>
        /// <remarks>Without an address the same as <see cref="Recv"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Recv"/>
        WinsockErrorCode RecvFrom(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            return Recv(buffers, bufferCount, bytesTransferred, timeout);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without capturing the sender's address.</summary>
        /// <remarks>Without an address the same as <see cref="RecvAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="RecvAsync"/>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags,
                                   AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return RecvAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecvFrom</c>.</summary>
        /// <remarks>Without an address the same as <see cref="Recv"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Recv"/>
        WinsockErrorCode RecvFrom(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags,
                                  Duration timeout = Duration::Infinite()) noexcept
        {
            return Recv(buffer, bufferSize, bytesTransferred, flags, timeout);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without flags or sender address.</summary>
        /// <remarks>Without an address the same as <see cref="RecvAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="RecvAsync"/>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return RecvAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecvFrom</c> without flags or sender address.</summary>
        /// <remarks>Without an address the same as <see cref="Recv"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Recv"/>
        WinsockErrorCode RecvFrom(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            return Recv(buffer, bufferSize, bytesTransferred, timeout);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call. The result flags (e.g. <c>MSG_PARTIAL</c>) are stored in
        /// <paramref name="asyncIoRequest"/> (<see cref="AsyncSocketRecvIoRequest::GetFlags"/>) once the operation has completed.</param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address in bytes on completion; must stay valid until then.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned receive request that is completed with the operation's result and keeps its result flags. It must
        /// not be in use by another operation and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr, int& addrLength,
                                       AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvFromAsync(buffers, bufferCount, bytesTransferred, flags, addr, addrLength, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecvFrom</c>, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call; out on success: the result flags (e.g. <c>MSG_PARTIAL</c>).</param>
        /// <param name="addr">Receives the sender's address once the operation has completed.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFrom(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr,
                                  Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketRecvIoRequest_s asyncIoRequest(GetSocket());
            int                                addrLength; // written at completion; lives until the wait returns

            auto error = RecvFromAsync(buffers, bufferCount, bytesTransferred, flags, addr, addrLength, asyncIoRequest);
            error = WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
            if (WinsockError::Succeeded(error))
                *flags = asyncIoRequest.GetFlags();
            return error;
        }

        /// <summary>Starts a <c>WSARecvFrom</c> without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address in bytes on completion; must stay valid until then.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// <paramref name="addr"/> (the sender's address) and <paramref name="addrLength"/> are written at
        /// completion and must stay valid until then.
        /// </remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, _SockAddr& addr, int& addrLength,
                                       AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvFromAsync(buffers, bufferCount, bytesTransferred, addr, addrLength, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecvFrom</c> without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the receive buffers.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="addr">Receives the sender's address once the operation has completed.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFrom(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, _SockAddr& addr,
                                  Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;
            int                            addrLength; // written at completion; lives until the wait returns

            auto error = RecvFromAsync(buffers, bufferCount, bytesTransferred, addr, addrLength, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The receive buffer; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call. The result flags (e.g. <c>MSG_PARTIAL</c>) are stored in
        /// <paramref name="asyncIoRequest"/> (<see cref="AsyncSocketRecvIoRequest::GetFlags"/>) once the operation has completed.</param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address in bytes on completion; must stay valid until then.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned receive request that is completed with the operation's result and keeps its result flags. It must
        /// not be in use by another operation and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr, int& addrLength,
                                       AsyncSocketRecvIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvFromAsync(buffer, bufferSize, bytesTransferred, flags, addr, addrLength, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecvFrom</c>, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The receive buffer.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags">Required. In: <c>MSG_*</c> flags for the call; out on success: the result flags (e.g. <c>MSG_PARTIAL</c>).</param>
        /// <param name="addr">Receives the sender's address once the operation has completed.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFrom(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr,
                                  Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketRecvIoRequest_s asyncIoRequest(GetSocket());
            int                                addrLength; // written at completion; lives until the wait returns

            auto error = RecvFromAsync(buffer, bufferSize, bytesTransferred, flags, addr, addrLength, asyncIoRequest);
            error = WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
            if (WinsockError::Succeeded(error))
                *flags = asyncIoRequest.GetFlags();
            return error;
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The receive buffer; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address in bytes on completion; must stay valid until then.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// The buffer, <paramref name="addr"/> (the sender's address) and <paramref name="addrLength"/> are
        /// written at completion and must stay valid until then.
        /// </remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, _SockAddr& addr, int& addrLength,
                                       AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvFromAsync(buffer, bufferSize, bytesTransferred, addr, addrLength, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives into one buffer with <c>WSARecvFrom</c> without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The receive buffer.</param>
        /// <param name="bufferSize">Size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes received once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="addr">Receives the sender's address once the operation has completed.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode RecvFrom(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, _SockAddr& addr,
                                  Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;
            int                            addrLength; // written at completion; lives until the wait returns

            auto error = RecvFromAsync(buffer, bufferSize, bytesTransferred, addr, addrLength, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        using SocketBase::SendToAsync;

        /// <summary>Starts a <c>WSASendTo</c> without address.</summary>
        /// <remarks>Without an address the same as <see cref="SendAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="SendAsync"/>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return SendAsync(buffers, bufferCount, bytesTransferred, flags, asyncIoRequest);
        }

        /// <summary>Sends with <c>WSASendTo</c> without address.</summary>
        /// <remarks>Without an address the same as <see cref="Send"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Send"/>
        WinsockErrorCode SendTo(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, Duration timeout = Duration::Infinite()) noexcept
        {
            return Send(buffers, bufferCount, bytesTransferred, flags, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> without address or flags.</summary>
        /// <remarks>Without an address the same as <see cref="SendAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="SendAsync"/>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return SendAsync(buffers, bufferCount, bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends with <c>WSASendTo</c> without address or flags.</summary>
        /// <remarks>Without an address the same as <see cref="Send"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Send"/>
        WinsockErrorCode SendTo(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            return Send(buffers, bufferCount, bytesTransferred, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer without address.</summary>
        /// <remarks>Without an address the same as <see cref="SendAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="SendAsync"/>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return SendAsync(buffer, bufferSize, bytesTransferred, flags, asyncIoRequest);
        }

        /// <summary>Sends one buffer with <c>WSASendTo</c> without address.</summary>
        /// <remarks>Without an address the same as <see cref="Send"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Send"/>
        WinsockErrorCode SendTo(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags,
                                Duration timeout = Duration::Infinite()) noexcept
        {
            return Send(buffer, bufferSize, bytesTransferred, flags, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer without address or flags.</summary>
        /// <remarks>Without an address the same as <see cref="SendAsync"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="SendAsync"/>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return SendAsync(buffer, bufferSize, bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends one buffer with <c>WSASendTo</c> without address or flags.</summary>
        /// <remarks>Without an address the same as <see cref="Send"/> with the same arguments, which it calls.</remarks>
        /// <seealso cref="Send"/>
        WinsockErrorCode SendTo(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            return Send(buffer, bufferSize, bytesTransferred, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> to an address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="addr">
        /// The destination address, passed with the size of <typeparamref name="_SockAddr"/>; must stay valid until the
        /// operation has completed.
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                     AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendToAsync(buffers, bufferCount, bytesTransferred, flags, addr, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends to an address with <c>WSASendTo</c>.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="addr">The destination address, passed with the size of <typeparamref name="_SockAddr"/>.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendTo(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendToAsync(buffers, bufferCount, bytesTransferred, flags, addr, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> to an address without flags.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">
        /// Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send. The array may be
        /// temporary; the memory it describes must stay valid until the operation has completed.
        /// </param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="addr">
        /// The destination address, passed with the size of <typeparamref name="_SockAddr"/>; must stay valid until the
        /// operation has completed.
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, const _SockAddr& addr, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendToAsync(buffers, bufferCount, bytesTransferred, addr, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends to an address with <c>WSASendTo</c> without flags.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffers">Array of <paramref name="bufferCount"/> <c>WSABUF</c> structures describing the data to send.</param>
        /// <param name="bufferCount">Number of <c>WSABUF</c> structures in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="addr">The destination address, passed with the size of <typeparamref name="_SockAddr"/>.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendTo(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, const _SockAddr& addr,
                                Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendToAsync(buffers, bufferCount, bytesTransferred, addr, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer to an address.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="addr">
        /// The destination address, passed with the size of <typeparamref name="_SockAddr"/>; must stay valid until the
        /// operation has completed.
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                     AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendToAsync(buffer, bufferSize, bytesTransferred, flags, addr, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends one buffer to an address with <c>WSASendTo</c>.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c> (e.g. <c>MSG_DONTROUTE</c>, <c>MSG_OOB</c>); <c>0</c> for none.</param>
        /// <param name="addr">The destination address, passed with the size of <typeparamref name="_SockAddr"/>.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendTo(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendToAsync(buffer, bufferSize, bytesTransferred, flags, addr, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer to an address without flags.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The data to send; must stay valid until the operation has completed.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="addr">
        /// The destination address, passed with the size of <typeparamref name="_SockAddr"/>; must stay valid until the
        /// operation has completed.
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, const _SockAddr& addr, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendToAsync(buffer, bufferSize, bytesTransferred, addr, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends one buffer to an address with <c>WSASendTo</c> without flags.</summary>
        /// <typeparam name="_SockAddr">
        /// The address type (<see cref="Windows::SockAddrType"/>): <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or
        /// <c>sockaddr_storage</c>.
        /// </typeparam>
        /// <param name="buffer">The data to send.</param>
        /// <param name="bufferSize">Number of bytes to send from <paramref name="buffer"/>.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="addr">The destination address, passed with the size of <typeparamref name="_SockAddr"/>.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        template <Windows::SockAddrType _SockAddr>
        WinsockErrorCode SendTo(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, const _SockAddr& addr,
                                Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendToAsync(buffer, bufferSize, bytesTransferred, addr, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        using SocketBase::RecvMsgAsync;
        using SocketBase::SendMsgAsync;
        using SocketBase::TransmitFileAsync;
        using SocketBase::TransmitPacketsAsync;

        /// <summary>Starts a <c>WSARecvMsg</c>.</summary>
        /// <param name="msg">Buffers, name and control space; <c>WSARecvMsg</c> writes into it at completion, so it must stay valid until then.</param>
        /// <param name="bytesTransferred">Required. Receives the bytes received on synchronous completion.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>
        /// For datagram/raw sockets; needs <see cref="Windows::Socket::Startup"/> for the extension pointer.
        /// </remarks>
        /// <seealso cref="RecvMsg"/>
        WinsockErrorCode RecvMsgAsync(WSAMSG* msg, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(RecvMsgAsync(msg, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Receives with <c>WSARecvMsg</c>.</summary>
        /// <param name="msg">Buffers, name and control space; <c>WSARecvMsg</c> fills it in once the operation has completed.</param>
        /// <param name="bytesTransferred">Required. Receives the bytes received once the operation has completed (<c>0</c> on failure).</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        /// <seealso cref="RecvMsgAsync"/>
        WinsockErrorCode RecvMsg(WSAMSG* msg, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = RecvMsgAsync(msg, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>WSASendMsg</c>.</summary>
        /// <param name="msg">The message (buffers, destination, control data); it and the memory it describes must stay valid until completion.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendMsg</c> (e.g. <c>MSG_DONTROUTE</c>); <c>0</c> for none.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent on synchronous completion; may be <c>nullptr</c> (the request then takes
        /// the count from the completed operation).
        /// </param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <seealso cref="SendMsg"/>
        WinsockErrorCode SendMsgAsync(WSAMSG* msg, DWORD flags, DWORD* bytesTransferred, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            return HandleCompletedRequest(SendMsgAsync(msg, flags, bytesTransferred, asyncIoRequest.GetOverlapped()), bytesTransferred, asyncIoRequest);
        }

        /// <summary>Sends with <c>WSASendMsg</c>.</summary>
        /// <param name="msg">The message (buffers, destination, control data).</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendMsg</c> (e.g. <c>MSG_DONTROUTE</c>); <c>0</c> for none.</param>
        /// <param name="bytesTransferred">
        /// Receives the bytes sent once the operation has completed (<c>0</c> on failure); may be <c>nullptr</c>.
        /// </param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        /// <seealso cref="SendMsgAsync"/>
        WinsockErrorCode SendMsg(WSAMSG* msg, DWORD flags, DWORD* bytesTransferred, Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = SendMsgAsync(msg, flags, bytesTransferred, asyncIoRequest);
            return WaitForAsyncIoOperation(error, bytesTransferred, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>TransmitFile</c> on a connected socket.</summary>
        /// <param name="file">
        /// The open file to send; <c>nullptr</c> sends only <paramref name="transmitBuffers"/>. Sending starts at the file
        /// offset in the request's <c>OVERLAPPED</c> (<c>0</c> after construction or <see cref="AsyncIoRequest::Reset"/>).
        /// </param>
        /// <param name="bytesToWrite">Bytes of the file to send; <c>0</c> sends the whole file.</param>
        /// <param name="bytesPerSend">Size of each send; <c>0</c> lets the system choose.</param>
        /// <param name="transmitBuffers">Optional head/tail data; must stay valid until completion.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <param name="flags"><c>TF_*</c> flags, e.g. <c>TF_DISCONNECT</c> | <c>TF_REUSE_SOCKET</c>; <c>0</c> (the default) for none.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <remarks>The file handle stays owned by the caller and must stay open until completion.</remarks>
        /// <seealso cref="TransmitFile"/>
        WinsockErrorCode TransmitFileAsync(HANDLE file, DWORD bytesToWrite, DWORD bytesPerSend, TRANSMIT_FILE_BUFFERS* transmitBuffers,
                                           AsyncSocketIoRequest& asyncIoRequest, DWORD flags = 0) noexcept
        {
            return HandleCompletedRequest(SocketBase::TransmitFileAsync(file, bytesToWrite, bytesPerSend, transmitBuffers, asyncIoRequest.GetOverlapped(), flags), nullptr, asyncIoRequest);
        }

        /// <summary>Sends a file with <c>TransmitFile</c>.</summary>
        /// <param name="file">
        /// The open file to send, starting at offset <c>0</c>; <c>nullptr</c> sends only <paramref name="transmitBuffers"/>.
        /// </param>
        /// <param name="bytesToWrite">Bytes of the file to send; <c>0</c> sends the whole file.</param>
        /// <param name="bytesPerSend">Size of each send; <c>0</c> lets the system choose.</param>
        /// <param name="transmitBuffers">Optional head/tail data; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>TF_*</c> flags, e.g. <c>TF_DISCONNECT</c> | <c>TF_REUSE_SOCKET</c>; <c>0</c> (the default) for none.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task until the operation has completed. The file handle stays owned
        /// by the caller.
        /// </remarks>
        /// <seealso cref="TransmitFileAsync"/>
        WinsockErrorCode TransmitFile(HANDLE file, DWORD bytesToWrite, DWORD bytesPerSend, TRANSMIT_FILE_BUFFERS* transmitBuffers, DWORD flags = 0,
                                      Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = TransmitFileAsync(file, bytesToWrite, bytesPerSend, transmitBuffers, asyncIoRequest, flags);
            return WaitForAsyncIoOperation(error, nullptr, asyncIoRequest, timeout);
        }

        /// <summary>Starts a <c>TransmitPackets</c> on a connected socket.</summary>
        /// <param name="packetArray">
        /// The elements (memory and/or file ranges); they and everything they reference must stay valid until completion.
        /// </param>
        /// <param name="elementCount">Number of elements in <paramref name="packetArray"/>.</param>
        /// <param name="transmitSize">Bytes per send; <c>0</c> lets the system choose.</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <param name="flags"><c>TF_*</c> flags, e.g. <c>TF_DISCONNECT</c> | <c>TF_REUSE_SOCKET</c>; <c>0</c> (the default) for none.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see the
        /// class remarks).
        /// </returns>
        /// <seealso cref="TransmitPackets"/>
        WinsockErrorCode TransmitPacketsAsync(TRANSMIT_PACKETS_ELEMENT* packetArray, DWORD elementCount, DWORD transmitSize,
                                              AsyncSocketIoRequest& asyncIoRequest, DWORD flags = 0) noexcept
        {
            return HandleCompletedRequest(SocketBase::TransmitPacketsAsync(packetArray, elementCount, transmitSize, asyncIoRequest.GetOverlapped(), flags), nullptr, asyncIoRequest);
        }

        /// <summary>Sends packets with <c>TransmitPackets</c>.</summary>
        /// <param name="packetArray">The elements (memory and/or file ranges) to send.</param>
        /// <param name="elementCount">Number of elements in <paramref name="packetArray"/>.</param>
        /// <param name="transmitSize">Bytes per send; <c>0</c> lets the system choose.</param>
        /// <param name="flags"><c>TF_*</c> flags, e.g. <c>TF_DISCONNECT</c> | <c>TF_REUSE_SOCKET</c>; <c>0</c> (the default) for none.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if <paramref name="timeout"/> expired and
        /// the operation was cancelled, or the operation's WSA error.
        /// </returns>
        /// <remarks>Task only; suspends the calling task until the operation has completed.</remarks>
        /// <seealso cref="TransmitPacketsAsync"/>
        WinsockErrorCode TransmitPackets(TRANSMIT_PACKETS_ELEMENT* packetArray, DWORD elementCount, DWORD transmitSize, DWORD flags = 0,
                                         Duration timeout = Duration::Infinite()) noexcept
        {
            WaitableAsyncSocketIoRequest_s asyncIoRequest;

            auto error = TransmitPacketsAsync(packetArray, elementCount, transmitSize, asyncIoRequest, flags);
            return WaitForAsyncIoOperation(error, nullptr, asyncIoRequest, timeout);
        }

    protected:
        /// <summary>Finishes the start of an operation.</summary>
        /// <typeparam name="_Request">
        /// The request type (<see cref="AsyncSocketIoRequestType"/>); a receive request (<see cref="AsyncSocketRecvIoRequest"/>)
        /// fetches its result flags itself when it is completed.
        /// </typeparam>
        /// <param name="error">
        /// The result of the starting call: <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or a
        /// synchronous WSA error.
        /// </param>
        /// <param name="bytesTransferred">
        /// The count the starting call wrote, or <c>nullptr</c>. The request is completed with *bytesTransferred,
        /// or without it with the count fetched from the operation.
        /// </param>
        /// <param name="asyncIoRequest">The request the operation was started with; completed in place on synchronous success.</param>
        /// <returns>
        /// If <paramref name="error"/> is <see cref="WinsockError::Success"/> (synchronous completion),
        /// completes <paramref name="asyncIoRequest"/> in place and returns Success; otherwise returns error
        /// unchanged (<see cref="WinsockError::Pending"/> or a synchronous failure).
        /// </returns>
        template <AsyncSocketIoRequestType _Request>
        WinsockErrorCode HandleCompletedRequest(WinsockErrorCode error, DWORD* bytesTransferred, _Request& asyncIoRequest) noexcept
        {
            // Only a synchronous success is completed here. A pending operation must stay pending, so
            // that the caller waits for its completion packet before the request (often on the stack)
            // goes away.
            if (WinsockError::Succeeded(error)) {
                DWORD transferred = 0;
                if (bytesTransferred) {
                    transferred = *bytesTransferred;
                } else {
                    // the caller passed no count (as recommended for overlapped calls): take it from
                    // the completed operation
                    DWORD flags = 0;
                    GetOverlappedResult(asyncIoRequest, &transferred, &flags);
                }

                asyncIoRequest.OnRequestCompletedImmediately(transferred);
                return WinsockError::Success;
            }

            return error;
        }

        /// <summary>Waits for an operation started with <paramref name="asyncIoRequest"/>.</summary>
        /// <typeparam name="_Request">
        /// The waitable request type (<see cref="WaitableAsyncSocketIoRequestType"/>), e.g.
        /// <see cref="WaitableAsyncSocketIoRequest_s"/>.
        /// </typeparam>
        /// <param name="error">The result of the starting XxxAsync call; only <see cref="WinsockError::Pending"/> waits.</param>
        /// <param name="bytesTransferred">
        /// Receives the request's transferred bytes on success, <c>0</c> on failure; may be <c>nullptr</c>.
        /// </param>
        /// <param name="asyncIoRequest">The request the operation was started with; must live in the caller's frame until this returns.</param>
        /// <param name="timeout">
        /// Maximum time to wait; <see cref="Duration::Infinite"/> (the default) waits without limit. A finite timeout needs
        /// <see cref="EnableTimers"/>; on expiry the operation is cancelled.
        /// </param>
        /// <returns>
        /// The operation's final error: <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/> if
        /// <paramref name="timeout"/> expired and the operation was cancelled, or the WSA error.
        /// </returns>
        /// <remarks>
        /// If <paramref name="error"/> is <see cref="WinsockError::Pending"/>, suspends the calling task until
        /// the request completes (or <paramref name="timeout"/> expires, then cancels and returns
        /// <see cref="WinsockError::TimedOut"/>, see the class remarks); then stores the bytes transferred
        /// (<c>0</c> on failure). Task only. The request must live in the caller's frame until this returns.
        /// </remarks>
        template <WaitableAsyncSocketIoRequestType _Request>
        WinsockErrorCode WaitForAsyncIoOperation(WinsockErrorCode error, DWORD* bytesTransferred, _Request& asyncIoRequest,
                                                 Duration timeout = Duration::Infinite()) noexcept
        {
            if (WinsockError::IsPending(error)) {
                if (timeout.IsInfinite())
                    asyncIoRequest.Wait();
                else if (!asyncIoRequest.Wait(timeout)) {
                    // Timed out: cancel the operation, then wait for its completion without a limit -
                    // the OVERLAPPED lives in this frame. If it completed meanwhile, its result stands.
                    // A cancelled socket operation leaves the socket fit for closing only.
                    CancelIoEx(reinterpret_cast<HANDLE>(GetSocket()), &asyncIoRequest.GetOverlapped());
                    asyncIoRequest.Wait();
                    if (asyncIoRequest.GetError() == WinsockError::OperationAborted) {
                        if (bytesTransferred)
                            *bytesTransferred = 0;
                        return WinsockError::TimedOut;
                    }
                }
                error = asyncIoRequest.GetError();
            }

            if (bytesTransferred)
                *bytesTransferred = WinsockError::Succeeded(error) ? asyncIoRequest.GetTransferredBytes() : 0;

            return error;
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::AsyncIo