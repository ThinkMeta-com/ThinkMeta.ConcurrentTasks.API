//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// TCP layer over IpSocketBase: Create() without arguments creates a SOCK_STREAM/IPPROTO_TCP
// socket of the version's address family. TcpSocket uses plain Socket; AsyncTcpSocket builds on
// TcpSocketBase with AsyncSocket.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>
#include <Windows.h>
#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.TcpSocket;

export import ThinkMeta.ConcurrentTasks.Windows.IpSocket;

namespace ThinkMeta::ConcurrentTasks::Windows {
    // Unlike IpSocketBase, no per-version specializations: nothing TCP adds here depends on the
    // IP version.

    /// <summary>An IPv4 or IPv6 TCP socket.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <typeparam name="_BaseSocket">
    /// The socket class built on: <see cref="Socket"/> (blocking calls) or <see cref="AsyncIo::AsyncSocket"/>.
    /// </typeparam>
    /// <remarks>
    /// Plain (<see cref="Socket"/>) or asynchronous (<see cref="AsyncIo::AsyncSocket"/>). Owns its socket
    /// like Socket.
    /// </remarks>
    /// <seealso cref="TcpSocket"/>
    /// <seealso cref="AsyncIo::AsyncTcpSocket"/>
    export template <IpVersionType _IpVersion, SocketType _BaseSocket>
    class TcpSocketBase : public IpSocketBase<_IpVersion, _BaseSocket>
    {
        using _IpSocketBase = IpSocketBase<_IpVersion, _BaseSocket>;
        using _Traits = IpTraits<_IpVersion>;

    public:
        /// <summary>The version's socket address type (<c>sockaddr_in</c> or <c>sockaddr_in6</c>).</summary>
        using SockAddr = typename _Traits::SockAddr;

        /// <summary>Creates an object without a socket.</summary>
        TcpSocketBase() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        TcpSocketBase(TcpSocketBase&& other) noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; a socket this object already holds is closed first.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        /// <returns>This object.</returns>
        TcpSocketBase& operator=(TcpSocketBase&& other) noexcept = default;

        // Keep the base's Create(addressFamily, type, protocol, ...) visible next to the TCP overload.
        using _IpSocketBase::Create;

        /// <summary>Creates a TCP socket of the version's address family.</summary>
        /// <param name="protocolInfo">
        /// Optional <c>WSAPROTOCOL_INFO</c> that selects the provider; <c>nullptr</c> for the default.
        /// </param>
        /// <param name="g">The socket group (<c>0</c> for none, which is the usual value).</param>
        /// <param name="flags">
        /// <c>WSA_FLAG_*</c> flags, e.g. <c>WSA_FLAG_OVERLAPPED</c> (an asynchronous socket adds it itself).
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, or the WSA error of <c>WSASocket</c>; for an asynchronous
        /// socket also the error of the completion port association (see <see cref="AsyncIo::AsyncSocket::Create"/>).
        /// </returns>
        /// <remarks>
        /// Parameters as <c>WSASocket</c>. Needs <see cref="Socket::Startup"/>. An asynchronous socket
        /// (<see cref="AsyncIo::AsyncTcpSocket"/>) is created overlapped and associated with the core's
        /// completion port, which needs <see cref="AsyncIo::EnableAsyncIo"/>.
        /// </remarks>
        /// <seealso cref="Socket::Close"/>
        WinsockErrorCode Create(WSAPROTOCOL_INFO* protocolInfo = nullptr, GROUP g = 0, DWORD flags = 0) noexcept
        {
            return this->Create(_Traits::AddressFamily, SOCK_STREAM, IPPROTO_TCP, protocolInfo, g, flags);
        }
    };

    /// <summary>A plain (non-task-aware) TCP socket of the given version.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <seealso cref="AsyncIo::AsyncTcpSocket"/>
    export template <IpVersionType _IpVersion>
    using TcpSocket = TcpSocketBase<_IpVersion, Socket>;

    /// <summary>Plain IPv4 TCP socket.</summary>
    /// <seealso cref="TcpSocket6"/>
    export using TcpSocket4 = TcpSocket<IpVersion::V4>;

    /// <summary>Plain IPv6 TCP socket.</summary>
    /// <seealso cref="TcpSocket4"/>
    export using TcpSocket6 = TcpSocket<IpVersion::V6>;
}
