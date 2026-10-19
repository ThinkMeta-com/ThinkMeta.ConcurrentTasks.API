//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncTcpSocket: an IPv4/IPv6 TCP socket on top of AsyncSocket, with Accept and Connect
// helpers that wrap AcceptEx/ConnectEx and apply the update-context options afterwards.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>
#include <Windows.h>
#include <MSWSock.h>

#include <array>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;

export import ThinkMeta.ConcurrentTasks.Windows.TcpSocket;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIpSocket;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    using Windows::IpVersion;
    using Windows::IpVersionType;
    using Windows::TcpSocketBase;
    using Windows::IpTraits;
    using Windows::WinsockError;
    using Windows::WinsockErrorCode;

    /// <summary>A TCP socket for overlapped I/O from tasks.</summary>
    /// <typeparam name="_IpVersion">
    /// The IP version: <see cref="Windows::IpVersion::V4"/> or <see cref="Windows::IpVersion::V6"/>.
    /// </typeparam>
    /// <remarks>
    /// <see cref="AsyncSocket"/> plus the TCP/IP options of <see cref="Windows::TcpSocketBase"/>/
    /// <see cref="Windows::IpSocketBase"/>, for <see cref="Windows::IpVersion::V4"/> or V6.
    /// Create() (no arguments, see <see cref="Windows::_TcpSocket::Create"/>) creates an overlapped TCP socket
    /// of the right address family and associates it with the core's completion port; needs
    /// <see cref="Windows::Socket::Startup"/> and <see cref="EnableAsyncIo"/>.
    /// The object owns the <c>SOCKET</c> and closes it on destruction.
    /// All the remarks of AsyncSocket apply (task-only waiting forms, timeouts, buffer lifetime).
    /// </remarks>
    /// <seealso cref="Windows::TcpSocket"/>
    export template <IpVersionType _IpVersion>
    class AsyncTcpSocket : public TcpSocketBase<_IpVersion, AsyncSocket>
    {
        using _Base = TcpSocketBase<_IpVersion, AsyncSocket>;
        using _Traits = IpTraits<_IpVersion>;

        static const DWORD AddressBufferSize = sizeof(typename _Traits::SockAddr) + 16;

    public:
        /// <summary>Creates an object without a socket.</summary>
        AsyncTcpSocket() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        AsyncTcpSocket(AsyncTcpSocket&& other) noexcept : _Base(std::move(other)) {}

        /// <summary>Takes over <paramref name="other"/>'s socket; a socket this object already holds is closed first.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        /// <returns>This object.</returns>
        AsyncTcpSocket& operator=(AsyncTcpSocket&& other) noexcept
        {
            _Base::operator=(std::move(other));
            return *this;
        }

        /// <summary>Accepts one connection on this listening socket.</summary>
        /// <param name="acceptSocket">
        /// Takes the connection. Must not hold a socket yet; Accept creates it. On failure it is closed again.
        /// </param>
        /// <param name="address">Optional; receives the peer's address.</param>
        /// <param name="timeout">Limit for the wait; see <see cref="AsyncSocket"/> for the timeout rules.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/>, or the WSA error of the
        /// create/<c>AcceptEx</c>/<c>setsockopt</c> step.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task. This socket must be created, bound and listening
        /// (<see cref="Windows::Socket::Listen"/>). Uses <c>AcceptEx</c> without initial data, so it completes
        /// as soon as a connection arrives, then applies <c>SO_UPDATE_ACCEPT_CONTEXT</c> so that
        /// <c>getpeername</c>, <c>shutdown</c> etc. work on <paramref name="acceptSocket"/>.
        /// </remarks>
        /// <seealso cref="AsyncSocket::AcceptAsync"/>
        WinsockErrorCode Accept(AsyncTcpSocket& acceptSocket, typename _Traits::SockAddr* address = nullptr, Duration timeout = Duration::Infinite()) noexcept
        {
            CT_ASSERT(!acceptSocket.IsValid());

            auto error = acceptSocket.Create();
            if (WinsockError::Failed(error))
                return error;

            WaitableAsyncSocketIoRequest_s asyncIoRequest;
            std::array<std::byte, 2 * AddressBufferSize> buffer;
            DWORD bytesReceived = 0;

            error = this->AcceptAsync(acceptSocket, buffer.data(), 0, AddressBufferSize, AddressBufferSize, &bytesReceived, asyncIoRequest);
            error = this->WaitForAsyncIoOperation(error, &bytesReceived, asyncIoRequest, timeout);

            if (WinsockError::Failed(error)) {
                acceptSocket.Close();
                return error;
            }

            // AcceptEx does not update the accepted socket's context from the listening socket, so
            // the new socket stays "not connected" (WSAENOTCONN / 10057) for getpeername, recv, send,
            // shutdown, etc. until SO_UPDATE_ACCEPT_CONTEXT is applied.
            const SOCKET listenSocket = this->GetSocket();
            error                     = acceptSocket.SetOption(SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, listenSocket);
            if (WinsockError::Failed(error)) {
                acceptSocket.Close();
                return error;
            }

            if (address) {
                typename _Traits::SockAddr* localAddress = nullptr;
                typename _Traits::SockAddr* remoteAddress = nullptr;
                int localAddressLength = 0;
                int remoteAddressLength = 0;

                this->GetAcceptExSockaddrs(buffer.data(), 0, AddressBufferSize, AddressBufferSize,
                                           reinterpret_cast<sockaddr**>(&localAddress), &localAddressLength,
                                           reinterpret_cast<sockaddr**>(&remoteAddress), &remoteAddressLength);

                // only what AcceptEx stored: a shorter address (family mismatch on a dual-stack
                // listener) must not pull bytes from behind it
                const auto length = remoteAddressLength > 0 && static_cast<std::size_t>(remoteAddressLength) < sizeof(*address)
                                        ? static_cast<std::size_t>(remoteAddressLength)
                                        : sizeof(*address);
                ZeroMemory(address, sizeof(*address));
                CopyMemory(address, remoteAddress, length);
            }

            return error;
        }

        /// <summary>Connects to an address.</summary>
        /// <param name="address">The remote address and port (<c>sockaddr_in</c> or <c>sockaddr_in6</c>).</param>
        /// <param name="timeout">Limit for the wait; see <see cref="AsyncSocket"/> for the timeout rules.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::TimedOut"/>, or the WSA error of
        /// <c>ConnectEx</c>/<c>setsockopt</c>.
        /// </returns>
        /// <remarks>
        /// Task only; suspends the calling task until connected. Uses <c>ConnectEx</c>. The socket must be
        /// created and bound (<see cref="Windows::Socket::Bind"/>, e.g. to the any address, port <c>0</c>);
        /// <c>ConnectEx</c> fails with <c>WSAEINVAL</c> otherwise. Applies <c>SO_UPDATE_CONNECT_CONTEXT</c> after the
        /// connection.
        /// </remarks>
        /// <seealso cref="ConnectAsync"/>
        WinsockErrorCode Connect(const typename _Traits::SockAddr& address, Duration timeout = Duration::Infinite()) noexcept
        {
            CT_ASSERT(this->IsValid());

            WaitableAsyncSocketIoRequest_s asyncIoRequest;
            auto error = this->ConnectAsync(&address, sizeof(address), nullptr, 0, nullptr, asyncIoRequest);
            error      = this->WaitForAsyncIoOperation(error, nullptr, asyncIoRequest, timeout);
            if (WinsockError::Failed(error))
                return error;

            // ConnectEx leaves the socket's context without the connection state;
            // SO_UPDATE_CONNECT_CONTEXT sets it so that getpeername/getsockname/shutdown and option
            // queries behave correctly. This option takes no value, so it uses the valueless
            // SetOption overload.
            error = this->SetOption(SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT);
            return error;
        }

        using _Base::ConnectAsync;

        /// <summary>Starts a connect without initial data.</summary>
        /// <param name="address">The remote address and port (<c>sockaddr_in</c> or <c>sockaddr_in6</c>).</param>
        /// <param name="asyncIoRequest">
        /// The caller-owned request that is completed with the operation's result. It must not be in use by another operation
        /// and must stay valid and must not move until it has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Pending"/> or the WSA error (see
        /// <see cref="AsyncSocket"/>).
        /// </returns>
        /// <remarks>
        /// Never suspends; uses <c>ConnectEx</c>. The socket must be created and bound. After successful
        /// completion the caller calls <see cref="Windows::Socket::SetOptionUpdateConnectContext"/>.
        /// </remarks>
        /// <seealso cref="Connect"/>
        /// <seealso cref="AsyncSocketIoRequest"/>
        WinsockErrorCode ConnectAsync(const typename _Traits::SockAddr& address, AsyncSocketIoRequest& asyncIoRequest) noexcept
        {
            CT_ASSERT(this->IsValid());

            return this->ConnectAsync(&address, sizeof(address), nullptr, 0, nullptr, asyncIoRequest);
        }
    };

    /// <summary>Asynchronous IPv4 TCP socket.</summary>
    /// <seealso cref="AsyncTcpSocket6"/>
    export using AsyncTcpSocket4 = AsyncTcpSocket<IpVersion::V4>;

    /// <summary>Asynchronous IPv6 TCP socket.</summary>
    /// <seealso cref="AsyncTcpSocket4"/>
    export using AsyncTcpSocket6 = AsyncTcpSocket<IpVersion::V6>;
}
