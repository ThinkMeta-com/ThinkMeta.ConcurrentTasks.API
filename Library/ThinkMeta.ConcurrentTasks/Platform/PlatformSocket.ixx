//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The socket of the platform the library is built for, under one name for code that runs on every
// platform: Windows::Socket (Winsock) on Windows, Posix::Socket elsewhere, with the error names and
// the shutdown constants that differ between them. Portable code uses what both have:
// Startup/Cleanup, Create, Close, Bind, Listen, Connect, GetSockName, GetPeerName, Shutdown,
// SetOption/GetOption, and the errors through SocketError (WSA codes on Windows, errno elsewhere).
// The IP and TCP layers on top are the same everywhere and live in Net (Net.IpSocket, Net.TcpSocket).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#if defined(_WIN32)
#    include <WinSock2.h>
#else
#    include <sys/socket.h>
#endif

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Platform.Socket;

#if defined(CT_PLATFORM_WINDOWS)
export import ThinkMeta.ConcurrentTasks.Windows.Socket;
#else
export import ThinkMeta.ConcurrentTasks.Posix.Socket;
#endif

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>Names and predicates of the socket error codes: Winsock's on Windows, <c>errno</c> elsewhere.</summary>
    /// <seealso cref="Windows::WinsockError"/>
#if defined(CT_PLATFORM_WINDOWS)
    export using SocketError = Windows::WinsockError;
#else
    export using SocketError = Posix::Error;
#endif

    /// <summary>A socket error code; <see cref="SocketError::Success"/> on success.</summary>
    export using SocketErrorCode = SocketError::Type;

    /// <summary>
    /// Which side <c>Shutdown</c> ends: <c>SD_RECEIVE</c>/<c>SD_SEND</c>/<c>SD_BOTH</c> on Windows,
    /// <c>SHUT_RD</c>/<c>SHUT_WR</c>/<c>SHUT_RDWR</c> elsewhere.
    /// </summary>
    export struct SocketShutdown {
#if defined(CT_PLATFORM_WINDOWS)
        /// <summary>No more receiving.</summary>
        static constexpr int Receive = SD_RECEIVE;
        /// <summary>No more sending: the peer's receive returns 0 bytes once it has all data.</summary>
        static constexpr int Send = SD_SEND;
        /// <summary>Neither.</summary>
        static constexpr int Both = SD_BOTH;
#else
        /// <summary>No more receiving.</summary>
        static constexpr int Receive = SHUT_RD;
        /// <summary>No more sending: the peer's receive returns 0 bytes once it has all data.</summary>
        static constexpr int Send = SHUT_WR;
        /// <summary>Neither.</summary>
        static constexpr int Both = SHUT_RDWR;
#endif
    };

    /// <summary>Owns one socket of the platform.</summary>
    /// <seealso cref="Windows::Socket"/>
#if defined(CT_PLATFORM_WINDOWS)
    export using Socket = Windows::Socket;
#else
    export using Socket = Posix::Socket;
#endif
} // namespace ThinkMeta::ConcurrentTasks::Platform
