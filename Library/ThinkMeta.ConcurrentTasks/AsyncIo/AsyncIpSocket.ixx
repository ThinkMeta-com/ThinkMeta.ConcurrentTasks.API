//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AsyncIpSocket: the IP-level options of IpSocketBase on top of AsyncSocket, for sockets of any
// IP type (e.g. UDP or raw) that use overlapped I/O through the core's completion port (Windows)
// or io_uring (elsewhere).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.AsyncIo.AsyncIpSocket;

export import ThinkMeta.ConcurrentTasks.Net.IpSocket;
export import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocket;

namespace ThinkMeta::ConcurrentTasks::AsyncIo {
    using Net::IpVersion;
    using Net::IpVersionType;
    using Net::IpSocketBase;

    /// <summary>An IPv4/IPv6 socket for overlapped I/O from tasks.</summary>
    /// <typeparam name="_IpVersion">
    /// The IP version: <see cref="Net::IpVersion::V4"/> or <see cref="Net::IpVersion::V6"/>.
    /// </typeparam>
    /// <remarks>
    /// <see cref="AsyncSocket"/> plus the IP options of <see cref="Net::IpSocketBase"/>. Create it with
    /// <see cref="AsyncSocket::Create"/>(addressFamily, type, protocol); see AsyncSocket for the preconditions and
    /// the rules of the starting and waiting forms.
    /// </remarks>
    /// <seealso cref="AsyncTcpSocket"/>
    /// <seealso cref="Net::IpSocket"/>
    export template <IpVersionType _IpVersion>
    using AsyncIpSocket = IpSocketBase<_IpVersion, AsyncSocket>;

    /// <summary>Asynchronous IPv4 socket.</summary>
    /// <seealso cref="AsyncIpSocket6"/>
    export using AsyncIpSocket4 = AsyncIpSocket<IpVersion::V4>;

    /// <summary>Asynchronous IPv6 socket.</summary>
    /// <seealso cref="AsyncIpSocket4"/>
    export using AsyncIpSocket6 = AsyncIpSocket<IpVersion::V6>;
}
