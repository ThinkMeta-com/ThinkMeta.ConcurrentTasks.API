//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// IP-level layer over Socket/AsyncSocket: the IpVersion tags, IpTraits (address types, option
// level and option constants per version) and IpSocketBase with the IPPROTO_IP/IPPROTO_IPV6
// socket options. Used by TcpSocket, AsyncIpSocket and AsyncTcpSocket.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>

#include <Windows.h>
#include <ws2ipdef.h>

#include <concepts>
#include <cstdint>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.IpSocket;

export import ThinkMeta.ConcurrentTasks.Windows.Socket;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Tags selecting the IP version of the IP socket templates.</summary>
    export struct IpVersion {
        /// <summary>IPv4 (<c>AF_INET</c>, <c>sockaddr_in</c>).</summary>
        struct V4;

        /// <summary>IPv6 (<c>AF_INET6</c>, <c>sockaddr_in6</c>).</summary>
        struct V6;
    };

    /// <summary>Satisfied by <see cref="IpVersion::V4"/> and <see cref="IpVersion::V6"/>.</summary>
    /// <typeparam name="T">The type to check.</typeparam>
    export template <typename T>
    concept IpVersionType = std::same_as<T, IpVersion::V4> || std::same_as<T, IpVersion::V6>;

    /// <summary>The base socket type of the IP socket templates.</summary>
    /// <typeparam name="T">The type to check.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="Socket"/> and classes derived from it (e.g. <see cref="AsyncIo::AsyncSocket"/>).
    /// </remarks>
    export template <typename T>
    concept SocketType = std::derived_from<T, Socket>;

    /// <summary>Per-version types and constants of the IP sockets.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <remarks>
    /// SockAddr, Addr, MulticastGroupInfo, AddressFamily, the option level and the Option_* numbers, and
    /// AnyAddress() (the wildcard address).
    /// </remarks>
    /// <seealso cref="IpVersion"/>
    export template <IpVersionType _IpVersion>
    struct IpTraits;

    template <>
    struct IpTraits<IpVersion::V4> {
        using SockAddr           = sockaddr_in;
        using Addr               = in_addr;
        using MulticastGroupInfo = ip_mreq;

        static constexpr int AddressFamily = AF_INET;
        static constexpr int OptionLevel   = IPPROTO_IP;

        static constexpr int Option_AddMembership      = IP_ADD_MEMBERSHIP;
        static constexpr int Option_DropMembership     = IP_DROP_MEMBERSHIP;
        static constexpr int Option_HeaderInclude      = IP_HDRINCL;
        static constexpr int Option_MulticastInterface = IP_MULTICAST_IF;
        static constexpr int Option_MulticastLoop      = IP_MULTICAST_LOOP;
        static constexpr int Option_PacketInformation  = IP_PKTINFO;
        static constexpr int Option_RecvInterface      = IP_RECVIF;
        static constexpr int Option_UnicastInterface   = IP_UNICAST_IF;
        static constexpr int Option_HopLimit           = IP_HOPLIMIT;

        static constexpr Addr AnyAddress() noexcept { return Addr{0}; }
    };

    template <>
    struct IpTraits<IpVersion::V6> {
        using SockAddr           = sockaddr_in6;
        using Addr               = in6_addr;
        using MulticastGroupInfo = ipv6_mreq;

        static constexpr int AddressFamily = AF_INET6;
        static constexpr int OptionLevel   = IPPROTO_IPV6;

        static constexpr int Option_AddMembership      = IPV6_ADD_MEMBERSHIP;
        static constexpr int Option_DropMembership     = IPV6_DROP_MEMBERSHIP;
        static constexpr int Option_HeaderInclude      = IPV6_HDRINCL;
        static constexpr int Option_MulticastInterface = IPV6_MULTICAST_IF;
        static constexpr int Option_MulticastLoop      = IPV6_MULTICAST_LOOP;
        static constexpr int Option_PacketInformation  = IPV6_PKTINFO;
        static constexpr int Option_RecvInterface      = IPV6_RECVIF;
        static constexpr int Option_UnicastInterface   = IPV6_UNICAST_IF;
        static constexpr int Option_HopLimit           = IPV6_HOPLIMIT;

        static constexpr Addr AnyAddress() noexcept { return Addr{0}; }
    };

    /// <summary>The IP options common to IPv4 and IPv6.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <typeparam name="_BaseSocket">
    /// The socket class built on: <see cref="Socket"/> (blocking calls) or <see cref="AsyncIo::AsyncSocket"/>.
    /// </typeparam>
    /// <remarks>
    /// Builds on <see cref="Socket"/> or <see cref="AsyncIo::AsyncSocket"/>; available on every
    /// <see cref="IpSocketBase"/> (<see cref="IpSocket"/>, <see cref="AsyncIo::AsyncIpSocket"/>, the TCP sockets).
    /// Each option member is a <c>setsockopt</c>/<c>getsockopt</c> at the version's IP level; errors as
    /// <see cref="WinsockErrorCode"/>.
    /// </remarks>
    /// <seealso cref="Socket::SetOption"/>
    /// <seealso cref="Socket::GetOption"/>
    export template <IpVersionType _IpVersion, SocketType _BaseSocket = Socket>
    class CT_NOVTABLE _IpSocket : public _BaseSocket
    {
        using _Traits                = IpTraits<_IpVersion>;
        static constexpr int OptionLevel = _Traits::OptionLevel;

    protected:
        _IpSocket() noexcept = default;
        _IpSocket(_IpSocket&& other) noexcept            = default;
        _IpSocket& operator=(_IpSocket&& other) noexcept = default;

    public:
        /// <summary>Joins a multicast group (<c>IP_ADD_MEMBERSHIP</c> / <c>IPV6_ADD_MEMBERSHIP</c>).</summary>
        /// <param name="mgi">
        /// The multicast group and the local interface (<c>ip_mreq</c> for IPv4, <c>ipv6_mreq</c> for IPv6).
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionDropMembership"/>
        WinsockErrorCode SetOptionAddMembership(const typename _Traits::MulticastGroupInfo& mgi) noexcept
        {
            return this->SetOption(OptionLevel, _Traits::Option_AddMembership, mgi);
        }

        /// <summary>Leaves a multicast group (<c>IP_DROP_MEMBERSHIP</c> / <c>IPV6_DROP_MEMBERSHIP</c>).</summary>
        /// <param name="mgi">
        /// The multicast group and the local interface (<c>ip_mreq</c> for IPv4, <c>ipv6_mreq</c> for IPv6), as joined.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionAddMembership"/>
        WinsockErrorCode SetOptionDropMembership(const typename _Traits::MulticastGroupInfo& mgi) noexcept
        {
            return this->SetOption(OptionLevel, _Traits::Option_DropMembership, mgi);
        }

        /// <summary>Sets whether the application supplies the IP header (<c>IP_HDRINCL</c> / <c>IPV6_HDRINCL</c>).</summary>
        /// <param name="include"><c>true</c> if the data sent starts with the IP header, <c>false</c> if the stack adds it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>For raw sockets.</remarks>
        WinsockErrorCode SetOptionHeaderInclude(bool include) noexcept { return this->SetOption(OptionLevel, _Traits::Option_HeaderInclude, include); }

        /// <summary>Gets the header-include setting.</summary>
        /// <param name="include">Required. Receives <c>true</c> if the application supplies the IP header.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionHeaderInclude(bool* include) noexcept { return this->GetOption(OptionLevel, _Traits::Option_HeaderInclude, include); }

        /// <summary>Sets whether the hop limit of received packets is reported (<c>IP_HOPLIMIT</c>/<c>IPV6_HOPLIMIT</c>).</summary>
        /// <param name="enable"><c>true</c> to report the hop limit (TTL) in the control data, <c>false</c> to stop it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionHopLimit(bool enable) noexcept { return this->SetOption(OptionLevel, _Traits::Option_HopLimit, enable); }

        /// <summary>Gets whether the hop limit of received packets is reported.</summary>
        /// <param name="enable">Required. Receives <c>true</c> if the hop limit is reported.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionHopLimit(bool* enable) noexcept { return this->GetOption(OptionLevel, _Traits::Option_HopLimit, enable); }

        /// <summary>Sets the outgoing multicast interface (<c>IP_MULTICAST_IF</c> / <c>IPV6_MULTICAST_IF</c>).</summary>
        /// <param name="index">The interface index, in host byte order; <c>0</c> selects the default interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionMulticastInterface(DWORD index) noexcept
        {
            if constexpr (std::same_as<_IpVersion, IpVersion::V4>) {
                return this->SetOption(OptionLevel, _Traits::Option_MulticastInterface, htonl(index));
            } else {
                return this->SetOption(OptionLevel, _Traits::Option_MulticastInterface, index);
            }
        }

        /// <summary>Gets the outgoing multicast interface index.</summary>
        /// <param name="index">Required. Receives the interface index, in host byte order.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMulticastInterface(DWORD* index) noexcept
        {
            WinsockErrorCode error = this->GetOption(OptionLevel, _Traits::Option_MulticastInterface, index);
            if (WinsockError::Succeeded(error)) {
                if constexpr (std::same_as<_IpVersion, IpVersion::V4>) {
                    *index = ntohl(*index);
                }
            }
            return error;
        }

        /// <summary>Sets multicast loopback (<c>IP_MULTICAST_LOOP</c> / <c>IPV6_MULTICAST_LOOP</c>).</summary>
        /// <param name="enable"><c>true</c> if multicast sent is looped back to the local host, <c>false</c> otherwise.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionMulticastLoop(bool enable) noexcept { return this->SetOption(OptionLevel, _Traits::Option_MulticastLoop, enable); }

        /// <summary>Gets the multicast loopback setting.</summary>
        /// <param name="enable">Required. Receives <c>true</c> if multicast sent is looped back to the local host.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMulticastLoop(bool* enable) noexcept { return this->GetOption(OptionLevel, _Traits::Option_MulticastLoop, enable); }

        /// <summary>Enables reporting the original arrival interface (<c>IP_ORIGINAL_ARRIVAL_IF</c>).</summary>
        /// <param name="enable"><c>true</c> to report the original arrival interface, <c>false</c> to stop it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>Reported in received control data. Uses the IPv4 option number at the version's option level, also for IPv6.</remarks>
        WinsockErrorCode SetOptionOriginalArrivalInterface(bool enable) noexcept { return this->SetOption(OptionLevel, IP_ORIGINAL_ARRIVAL_IF, enable); }

        /// <summary>Gets the original-arrival-interface setting.</summary>
        /// <param name="enable">Required. Receives <c>true</c> if the original arrival interface is reported.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionOriginalArrivalInterface(bool* enable) noexcept { return this->GetOption(OptionLevel, IP_ORIGINAL_ARRIVAL_IF, enable); }

        /// <summary>Enables packet information (<c>IP_PKTINFO</c> / <c>IPV6_PKTINFO</c>).</summary>
        /// <param name="enable"><c>true</c> to report packet information, <c>false</c> to stop it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>
        /// Reported in received control data; use with <see cref="AsyncIo::AsyncSocket::RecvMsg"/>.
        /// </remarks>
        WinsockErrorCode SetOptionPacketInformation(bool enable) noexcept { return this->SetOption(OptionLevel, _Traits::Option_PacketInformation, enable); }

        /// <summary>Gets the packet-information setting.</summary>
        /// <param name="enable">Required. Receives <c>true</c> if packet information is reported.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionPacketInformation(bool* enable) noexcept { return this->GetOption(OptionLevel, _Traits::Option_PacketInformation, enable); }

        /// <summary>Enables reporting the receiving interface (<c>IP_RECVIF</c> / <c>IPV6_RECVIF</c>).</summary>
        /// <param name="enable">
        /// <c>true</c>: <c>WSARecvMsg</c> returns the interface a datagram arrived on in its control data.
        /// Datagram and raw sockets only.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionRecvInterface(bool enable) noexcept { return this->SetOption(OptionLevel, _Traits::Option_RecvInterface, enable); }

        /// <summary>Gets whether the receiving interface is reported (<c>IP_RECVIF</c> / <c>IPV6_RECVIF</c>).</summary>
        /// <param name="enable">Required. Receives <c>true</c> if the receiving interface is reported.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionRecvInterface(bool* enable) noexcept
        {
            return this->GetOption(OptionLevel, _Traits::Option_RecvInterface, enable);
        }

        /// <summary>Sets the outgoing unicast interface (<c>IP_UNICAST_IF</c> / <c>IPV6_UNICAST_IF</c>).</summary>
        /// <param name="index">The interface index, in host byte order; <c>0</c> selects the default interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionUnicastInterface(DWORD index) noexcept
        {
            if constexpr (std::same_as<_IpVersion, IpVersion::V4>) {
                return this->SetOption(OptionLevel, _Traits::Option_UnicastInterface, htonl(index));
            } else {
                return this->SetOption(OptionLevel, _Traits::Option_UnicastInterface, index);
            }
        }

        /// <summary>Gets the outgoing unicast interface index.</summary>
        /// <param name="index">Required. Receives the interface index, in host byte order.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionUnicastInterface(DWORD* index) noexcept
        {
            WinsockErrorCode error = this->GetOption(OptionLevel, _Traits::Option_UnicastInterface, index);
            if (WinsockError::Succeeded(error)) {
                if constexpr (std::same_as<_IpVersion, IpVersion::V4>) {
                    *index = ntohl(*index);
                }
            }
            return error;
        }
    };

    /// <summary>An IPv4 or IPv6 socket with the IP options of that version.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <typeparam name="_BaseSocket">
    /// The socket class built on: <see cref="Socket"/> (blocking calls) or <see cref="AsyncIo::AsyncSocket"/>.
    /// </typeparam>
    /// <remarks>
    /// Plain (<see cref="Socket"/>) or asynchronous (<see cref="AsyncIo::AsyncSocket"/>). Owns its socket
    /// like Socket.
    /// </remarks>
    /// <seealso cref="IpSocket"/>
    /// <seealso cref="AsyncIo::AsyncIpSocket"/>
    export template <IpVersionType _IpVersion, SocketType _BaseSocket>
    class IpSocketBase;

    template <SocketType _BaseSocket>
    class IpSocketBase<IpVersion::V4, _BaseSocket> : public _IpSocket<IpVersion::V4, _BaseSocket>
    {
        using _IpSocketBase          = _IpSocket<IpVersion::V4, _BaseSocket>;
        using _Traits                = IpTraits<IpVersion::V4>;
        static constexpr int OptionLevel = _Traits::OptionLevel;

    public:
        /// <summary>Creates an object without a socket.</summary>
        IpSocketBase() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        IpSocketBase(IpSocketBase&& other) noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; a socket this object already holds is closed first.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        /// <returns>This object.</returns>
        IpSocketBase& operator=(IpSocketBase&& other) noexcept = default;

        // the overloads below would otherwise hide the base's ip_mreq forms
        using _IpSocketBase::SetOptionAddMembership;
        using _IpSocketBase::SetOptionDropMembership;

        /// <summary>Joins an IPv4 multicast group on an interface.</summary>
        /// <param name="multicastAddress">The IPv4 multicast group address.</param>
        /// <param name="index">The interface index (below 2^24, see remarks).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>The index is passed as interface address 0.x.y.z, which Windows reads as an interface index.</remarks>
        /// <seealso cref="SetOptionDropMembership"/>
        WinsockErrorCode SetOptionAddMembership(const in_addr& multicastAddress, DWORD index) noexcept
        {
            return _IpSocketBase::SetOptionAddMembership(MembershipByIndex(multicastAddress, index));
        }

        /// <summary>Leaves an IPv4 multicast group joined by interface index.</summary>
        /// <param name="multicastAddress">The IPv4 multicast group address, as joined.</param>
        /// <param name="index">The interface index, as joined.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionAddMembership"/>
        WinsockErrorCode SetOptionDropMembership(const in_addr& multicastAddress, DWORD index) noexcept
        {
            return _IpSocketBase::SetOptionDropMembership(MembershipByIndex(multicastAddress, index));
        }

        /// <summary>Joins a source-specific multicast group (<c>IP_ADD_SOURCE_MEMBERSHIP</c>).</summary>
        /// <param name="mgi">The multicast group, the source address to receive from and the local interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionDropSourceMembership"/>
        WinsockErrorCode SetOptionAddSourceMembership(const ip_mreq_source& mgi) noexcept
        {
            return this->SetOption(OptionLevel, IP_ADD_SOURCE_MEMBERSHIP, mgi);
        }

        /// <summary>Blocks a multicast source (<c>IP_BLOCK_SOURCE</c>).</summary>
        /// <param name="mgi">The joined multicast group, the source address to block and the local interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionUnblockSource"/>
        WinsockErrorCode SetOptionBlockSource(const ip_mreq_source& mgi) noexcept { return this->SetOption(OptionLevel, IP_BLOCK_SOURCE, mgi); }

        /// <summary>Sets the don't-fragment flag (<c>IP_DONTFRAGMENT</c>).</summary>
        /// <param name="dontFragment"><c>true</c> to send packets with the don't-fragment flag set.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDontFragment(bool dontFragment) noexcept { return this->SetOption(OptionLevel, IP_DONTFRAGMENT, dontFragment); }

        /// <summary>Gets the don't-fragment flag.</summary>
        /// <param name="dontFragment">Required. Receives <c>true</c> if packets are sent with the don't-fragment flag.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDontFragment(bool* dontFragment) noexcept { return this->GetOption(OptionLevel, IP_DONTFRAGMENT, dontFragment); }

        /// <summary>Leaves a source-specific multicast group (<c>IP_DROP_SOURCE_MEMBERSHIP</c>).</summary>
        /// <param name="mgi">The multicast group, the source address and the local interface, as joined.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionAddSourceMembership"/>
        WinsockErrorCode SetOptionDropSourceMembership(const ip_mreq_source& mgi) noexcept
        {
            return this->SetOption(OptionLevel, IP_DROP_SOURCE_MEMBERSHIP, mgi);
        }

        /// <summary>Sets the multicast TTL (<c>IP_MULTICAST_TTL</c>).</summary>
        /// <param name="ttl">The time to live of outgoing multicast packets (<c>0</c> to <c>255</c>; default <c>1</c>).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionMulticastTtl(DWORD ttl) noexcept { return this->SetOption(OptionLevel, IP_MULTICAST_TTL, ttl); }

        /// <summary>Gets the multicast TTL.</summary>
        /// <param name="ttl">Required. Receives the time to live of outgoing multicast packets.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMulticastTtl(DWORD* ttl) noexcept { return this->GetOption(OptionLevel, IP_MULTICAST_TTL, ttl); }

        /// <summary>Sets the IP options of outgoing packets (<c>IP_OPTIONS</c>).</summary>
        /// <param name="options">Required. The IP options as they appear in the header (at most 40 bytes).</param>
        /// <param name="length">The size of <paramref name="options"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionOptions(const char* options, int length) noexcept { return this->SetOption(OptionLevel, IP_OPTIONS, options, length); }

        /// <summary>Gets the IP options (<c>IP_OPTIONS</c>).</summary>
        /// <param name="options">Required. Receives the IP options.</param>
        /// <param name="length">Required. In: the size of <paramref name="options"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionOptions(char* options, int* length) noexcept { return this->GetOption(OptionLevel, IP_OPTIONS, options, length); }

        /// <summary>Sets whether broadcast is received (<c>IP_RECEIVE_BROADCAST</c>).</summary>
        /// <param name="enable"><c>true</c> to receive broadcast data, <c>false</c> to drop it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionReceiveBroadcast(bool enable) noexcept { return this->SetOption(OptionLevel, IP_RECEIVE_BROADCAST, enable); }

        /// <summary>Gets the receive-broadcast setting.</summary>
        /// <param name="enable">Required. Receives <c>true</c> if broadcast data is received.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionReceiveBroadcast(bool* enable) noexcept { return this->GetOption(OptionLevel, IP_RECEIVE_BROADCAST, enable); }

        /// <summary>Sets the type of service (<c>IP_TOS</c>).</summary>
        /// <param name="typeOfService">The type-of-service byte of outgoing packets.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionTypeOfService(DWORD typeOfService) noexcept { return this->SetOption(OptionLevel, IP_TOS, typeOfService); }

        /// <summary>Gets the type of service (<c>IP_TOS</c>).</summary>
        /// <param name="typeOfService">Required. Receives the type-of-service byte.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionTypeOfService(DWORD* typeOfService) noexcept { return this->GetOption(OptionLevel, IP_TOS, typeOfService); }

        /// <summary>Sets the unicast TTL (<c>IP_TTL</c>).</summary>
        /// <param name="ttl">The time to live of outgoing unicast packets (<c>1</c> to <c>255</c>).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionTtl(DWORD ttl) noexcept { return this->SetOption(OptionLevel, IP_TTL, ttl); }

        /// <summary>Gets the unicast TTL (<c>IP_TTL</c>).</summary>
        /// <param name="ttl">Required. Receives the time to live of outgoing unicast packets.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionTtl(DWORD* ttl) noexcept { return this->GetOption(OptionLevel, IP_TTL, ttl); }

        /// <summary>Unblocks a multicast source (<c>IP_UNBLOCK_SOURCE</c>).</summary>
        /// <param name="mgi">The multicast group, the blocked source address and the local interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionBlockSource"/>
        WinsockErrorCode SetOptionUnblockSource(const ip_mreq_source& mgi) noexcept { return this->SetOption(OptionLevel, IP_UNBLOCK_SOURCE, mgi); }

        /// <summary>Sets the WFP redirect context (<c>IP_WFP_REDIRECT_CONTEXT</c>).</summary>
        /// <param name="data">Required. The redirect context, as received in control data.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionWfpRedirectContext(const WSACMSGHDR* data, int length) noexcept
        {
            return this->SetOption(OptionLevel, IP_WFP_REDIRECT_CONTEXT, reinterpret_cast<const char*>(data), length);
        }

        /// <summary>Gets the WFP redirect context.</summary>
        /// <param name="data">Required. Receives the redirect context.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionWfpRedirectContext(WSACMSGHDR* data, int* length) noexcept
        {
            return this->GetOption(OptionLevel, IP_WFP_REDIRECT_CONTEXT, reinterpret_cast<char*>(data), length);
        }

        /// <summary>Sets the WFP redirect records (<c>IP_WFP_REDIRECT_RECORDS</c>).</summary>
        /// <param name="data">Required. The redirect records, as received in control data.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionWfpRedirectRecords(const WSACMSGHDR* data, int length) noexcept
        {
            return this->SetOption(OptionLevel, IP_WFP_REDIRECT_RECORDS, reinterpret_cast<const char*>(data), length);
        }

        /// <summary>Gets the WFP redirect records.</summary>
        /// <param name="data">Required. Receives the redirect records.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionWfpRedirectRecords(WSACMSGHDR* data, int* length) noexcept
        {
            return this->GetOption(OptionLevel, IP_WFP_REDIRECT_RECORDS, reinterpret_cast<char*>(data), length);
        }

        /// <summary>Sets family, address and port of a socket address.</summary>
        /// <param name="sockAddr">The socket address to fill; zero-initialize it first.</param>
        /// <param name="addr">The IPv4 address, e.g. <c>IpTraits&lt;IpVersion::V4&gt;::AnyAddress</c> (<c>INADDR_ANY</c>) for the wildcard address.</param>
        /// <param name="port">In host order; converted with <c>htons</c>.</param>
        /// <remarks>Other fields are not touched: zero-initialize <paramref name="sockAddr"/> first.</remarks>
        static void FillAddress(sockaddr_in& sockAddr, const in_addr& addr, std::uint16_t port) noexcept
        {
            sockAddr.sin_family = _Traits::AddressFamily;
            sockAddr.sin_addr   = addr;
            sockAddr.sin_port   = htons(port);
        }

    private:
        // interface address 0.x.y.z: Windows takes it as the interface index (network byte order)
        static ip_mreq MembershipByIndex(const in_addr& multicastAddress, DWORD index) noexcept
        {
            ip_mreq mgit{};
            mgit.imr_multiaddr        = multicastAddress;
            mgit.imr_interface.s_addr = htonl(index);
            return mgit;
        }
    };

    template <SocketType _BaseSocket>
    class IpSocketBase<IpVersion::V6, _BaseSocket> : public _IpSocket<IpVersion::V6, _BaseSocket>
    {
        using _IpSocketBase          = _IpSocket<IpVersion::V6, _BaseSocket>;
        using _Traits                = IpTraits<IpVersion::V6>;
        static constexpr int OptionLevel = _Traits::OptionLevel;

    public:
        /// <summary>Creates an object without a socket.</summary>
        IpSocketBase() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        IpSocketBase(IpSocketBase&& other) noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; a socket this object already holds is closed first.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        /// <returns>This object.</returns>
        IpSocketBase& operator=(IpSocketBase&& other) noexcept = default;

        // the overloads below would otherwise hide the base's ipv6_mreq forms
        using _IpSocketBase::SetOptionAddMembership;
        using _IpSocketBase::SetOptionDropMembership;

        /// <summary>Joins an IPv6 multicast group on an interface.</summary>
        /// <param name="multicastAddress">The IPv6 multicast group address.</param>
        /// <param name="index">The interface index; <c>0</c> selects the default interface.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionDropMembership"/>
        WinsockErrorCode SetOptionAddMembership(const in6_addr& multicastAddress, DWORD index) noexcept
        {
            const ipv6_mreq mgit = {multicastAddress, index};
            return _IpSocketBase::SetOptionAddMembership(mgit);
        }

        /// <summary>Leaves an IPv6 multicast group joined by interface index.</summary>
        /// <param name="multicastAddress">The IPv6 multicast group address, as joined.</param>
        /// <param name="index">The interface index, as joined.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="SetOptionAddMembership"/>
        WinsockErrorCode SetOptionDropMembership(const in6_addr& multicastAddress, DWORD index) noexcept
        {
            const ipv6_mreq mgit = {multicastAddress, index};
            return _IpSocketBase::SetOptionDropMembership(mgit);
        }

        /// <summary>Sets the unicast hop limit (<c>IPV6_UNICAST_HOPS</c>).</summary>
        /// <param name="hops">The hop limit of outgoing unicast packets (<c>0</c> to <c>255</c>).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>The IPv6 counterpart of the IPv4 time to live.</remarks>
        WinsockErrorCode SetOptionUnicastHops(DWORD hops) noexcept { return this->SetOption(OptionLevel, IPV6_UNICAST_HOPS, hops); }

        /// <summary>Gets the unicast hop limit (<c>IPV6_UNICAST_HOPS</c>).</summary>
        /// <param name="hops">Required. Receives the hop limit of outgoing unicast packets.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionUnicastHops(DWORD* hops) noexcept { return this->GetOption(OptionLevel, IPV6_UNICAST_HOPS, hops); }

        /// <summary>Sets the multicast hop limit (<c>IPV6_MULTICAST_HOPS</c>).</summary>
        /// <param name="hops">The hop limit of outgoing multicast packets (<c>0</c> to <c>255</c>; default <c>1</c>).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>The IPv6 counterpart of the IPv4 multicast time to live.</remarks>
        WinsockErrorCode SetOptionMulticastHops(DWORD hops) noexcept { return this->SetOption(OptionLevel, IPV6_MULTICAST_HOPS, hops); }

        /// <summary>Gets the multicast hop limit (<c>IPV6_MULTICAST_HOPS</c>).</summary>
        /// <param name="hops">Required. Receives the hop limit of outgoing multicast packets.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMulticastHops(DWORD* hops) noexcept { return this->GetOption(OptionLevel, IPV6_MULTICAST_HOPS, hops); }

        /// <summary>Sets the protection level (<c>IPV6_PROTECTION_LEVEL</c>, <c>PROTECTION_LEVEL_*</c>).</summary>
        /// <param name="level">
        /// <c>PROTECTION_LEVEL_UNRESTRICTED</c>, <c>PROTECTION_LEVEL_EDGERESTRICTED</c> or <c>PROTECTION_LEVEL_RESTRICTED</c>.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionProtectionLevel(INT level) noexcept { return this->SetOption(OptionLevel, IPV6_PROTECTION_LEVEL, level); }

        /// <summary>Gets the protection level.</summary>
        /// <param name="level">Required. Receives the <c>PROTECTION_LEVEL_*</c> value.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionProtectionLevel(INT* level) noexcept { return this->GetOption(OptionLevel, IPV6_PROTECTION_LEVEL, level); }

        /// <summary>Sets whether the socket is IPv6-only (<c>IPV6_V6ONLY</c>).</summary>
        /// <param name="ipv6Only">
        /// <c>true</c>: IPv6 only; <c>false</c>: allows dual-stack IPv4-mapped use.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>Set before <see cref="Socket::Bind"/>.</remarks>
        WinsockErrorCode SetOptionIpv6Only(bool ipv6Only) noexcept { return this->SetOption(OptionLevel, IPV6_V6ONLY, ipv6Only); }

        /// <summary>Gets the IPv6-only setting.</summary>
        /// <param name="ipv6Only">Required. Receives <c>true</c> if the socket is IPv6-only, <c>false</c> if dual-stack.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionIpv6Only(bool* ipv6Only) noexcept { return this->GetOption(OptionLevel, IPV6_V6ONLY, ipv6Only); }

        /// <summary>Sets family, address and port of a socket address.</summary>
        /// <param name="sockAddr">The socket address to fill; zero-initialize it first.</param>
        /// <param name="addr">The IPv6 address, e.g. <c>IpTraits&lt;IpVersion::V6&gt;::AnyAddress</c> (<c>in6addr_any</c>) for the wildcard address.</param>
        /// <param name="port">In host order; converted with <c>htons</c>.</param>
        /// <remarks>
        /// Other fields (flow info, scope id) are not touched: zero-initialize <paramref name="sockAddr"/> first.
        /// </remarks>
        static void FillAddress(sockaddr_in6& sockAddr, const in6_addr& addr, std::uint16_t port) noexcept
        {
            sockAddr.sin6_family = _Traits::AddressFamily;
            sockAddr.sin6_addr   = addr;
            sockAddr.sin6_port   = htons(port);
        }
    };

    /// <summary>A blocking (plain <see cref="Socket"/>) IP socket of the given version.</summary>
    /// <typeparam name="_IpVersion">The IP version: <see cref="IpVersion::V4"/> or <see cref="IpVersion::V6"/>.</typeparam>
    /// <seealso cref="AsyncIo::AsyncIpSocket"/>
    export template <IpVersionType _IpVersion>
    using IpSocket = IpSocketBase<_IpVersion, Socket>;

    /// <summary>Plain IPv4 socket.</summary>
    /// <seealso cref="IpSocket6"/>
    export using IpSocket4 = IpSocket<IpVersion::V4>;

    /// <summary>Plain IPv6 socket.</summary>
    /// <seealso cref="IpSocket4"/>
    export using IpSocket6 = IpSocket<IpVersion::V6>;
} // namespace ThinkMeta::ConcurrentTasks::Windows
