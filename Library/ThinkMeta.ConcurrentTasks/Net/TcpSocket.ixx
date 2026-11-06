//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// TCP layer over Net::IpSocketBase, the same on every platform: Create() without the type
// arguments creates a SOCK_STREAM/IPPROTO_TCP socket of the version's address family. TcpSocket uses
// Platform::Socket; AsyncTcpSocket builds on TcpSocketBase with AsyncSocket.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <WinSock2.h>

#    include <Windows.h>
#else
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/socket.h>
#endif

export module ThinkMeta.ConcurrentTasks.Net.TcpSocket;

export import ThinkMeta.ConcurrentTasks.Net.IpSocket;

namespace ThinkMeta::ConcurrentTasks::Net {
    using Platform::SocketErrorCode;

    // Unlike IpSocketBase, no per-version specializations: nothing TCP adds here depends on the
    // IP version.

    /// <summary>A TCP socket of the given IP version, on a plain or an asynchronous socket.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <typeparam name="_BaseSocket">
    /// The socket class built on: <see cref="Platform::Socket"/> (blocking calls) or <see cref="AsyncIo::AsyncSocket"/>.
    /// </typeparam>
    /// <remarks>
    /// Adds the TCP form of Create and the TCP options to <see cref="IpSocketBase"/>. Owns its socket like Socket.
    /// </remarks>
    /// <seealso cref="TcpSocket"/>
    /// <seealso cref="AsyncIo::AsyncTcpSocket"/>
    export template <IpVersionType _IpVersion, SocketType _BaseSocket>
    class TcpSocketBase : public IpSocketBase<_IpVersion, _BaseSocket>
    {
        using _IpSocketBase = IpSocketBase<_IpVersion, _BaseSocket>;
        using _Traits       = IpTraits<_IpVersion>;

    public:
        /// <summary>The socket address type of the IP version (<c>sockaddr_in</c> or <c>sockaddr_in6</c>).</summary>
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

#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>Creates a TCP socket of the IP version (<c>SOCK_STREAM</c>, <c>IPPROTO_TCP</c>).</summary>
        /// <param name="protocolInfo">Optional <c>WSAPROTOCOL_INFO</c> selecting the provider; <c>nullptr</c> for the default.</param>
        /// <param name="g">The Winsock socket group; <c>0</c> for none.</param>
        /// <param name="flags">Further <c>WSA_FLAG_*</c> flags; <c>0</c> for none.</param>
        /// <returns><see cref="Platform::SocketError::Success"/>, or the error of creating the socket.</returns>
        SocketErrorCode Create(WSAPROTOCOL_INFO* protocolInfo = nullptr, GROUP g = 0, DWORD flags = 0) noexcept
        {
            return this->Create(_Traits::AddressFamily, SOCK_STREAM, IPPROTO_TCP, protocolInfo, g, flags);
        }
#else
        /// <summary>Creates a TCP socket of the IP version (<c>SOCK_STREAM</c>, <c>IPPROTO_TCP</c>).</summary>
        /// <returns><see cref="Platform::SocketError::Success"/>, or the error of creating the socket.</returns>
        SocketErrorCode Create() noexcept { return this->Create(_Traits::AddressFamily, SOCK_STREAM, IPPROTO_TCP); }
#endif

        /// <summary>Sets <c>TCP_NODELAY</c>: send small segments at once instead of waiting to fill them (Nagle off).</summary>
        /// <param name="noDelay"><c>true</c> to send at once, <c>false</c> for the default (Nagle's algorithm).</param>
        /// <returns><see cref="Platform::SocketError::Success"/>, or the error of <c>setsockopt</c>.</returns>
        SocketErrorCode SetOptionNoDelay(bool noDelay) noexcept { return this->SetOption(IPPROTO_TCP, TCP_NODELAY, noDelay); }

        /// <summary>Gets the <c>TCP_NODELAY</c> setting.</summary>
        /// <param name="noDelay">Required. Receives <c>true</c> if Nagle's algorithm is off.</param>
        /// <returns><see cref="Platform::SocketError::Success"/>, or the error of <c>getsockopt</c>.</returns>
        SocketErrorCode GetOptionNoDelay(bool* noDelay) noexcept { return this->GetOption(IPPROTO_TCP, TCP_NODELAY, noDelay); }
    };

    /// <summary>A blocking (plain <see cref="Platform::Socket"/>) TCP socket of the given IP version.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <seealso cref="AsyncIo::AsyncTcpSocket"/>
    export template <IpVersionType _IpVersion>
    using TcpSocket = TcpSocketBase<_IpVersion, Platform::Socket>;

    /// <summary>A blocking TCP socket over IPv4.</summary>
    export using TcpSocket4 = TcpSocket<IpVersion::V4>;

    /// <summary>A blocking TCP socket over IPv6.</summary>
    export using TcpSocket6 = TcpSocket<IpVersion::V6>;
} // namespace ThinkMeta::ConcurrentTasks::Net
