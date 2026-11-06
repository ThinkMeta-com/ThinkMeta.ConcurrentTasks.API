//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Posix::Socket: a thin owner of a socket descriptor, the counterpart of Windows::Socket with the
// part that TCP over the core's asynchronous I/O needs: create, bind, listen, connect, addresses,
// shutdown and the common SOL_SOCKET options. Every function maps to one system call and returns
// its errno; nothing suspends a task. AsyncSocket adds the asynchronous operations (through the
// core, AsyncIo::SubmitIo).
//
// Not here, since Linux has no counterpart or AsyncSocket covers them: the Winsock extension
// functions (AcceptEx, ConnectEx, DisconnectEx, TransmitFile, TransmitPackets), the OVERLAPPED
// forms and the Winsock-only options.
//
// The file is not called Socket.ixx: file names stay unique across the library, since Visual
// Studio puts the objects of a project into one folder (MSB8027).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <concepts>
#include <cstdint>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Posix.Socket;

export import ThinkMeta.ConcurrentTasks.Posix.Error;

namespace ThinkMeta::ConcurrentTasks::Posix {
    /// <summary>The address structures the socket calls take.</summary>
    /// <typeparam name="T">The type to test.</typeparam>
    export template <typename T>
    concept SockAddrType = std::same_as<T, sockaddr> || std::same_as<T, sockaddr_in> || std::same_as<T, sockaddr_in6> || std::same_as<T, sockaddr_storage>;

    /// <summary>Owns one socket descriptor and closes it on destruction.</summary>
    /// <remarks>
    /// Holds <c>-1</c> when there is no socket. All members report failures as an <c>errno</c>
    /// (<see cref="ErrorCode"/>, <see cref="Error::Success"/> on success). Members that use the
    /// socket require one to be held (asserted in Debug builds). Plain blocking calls: called from
    /// a task they block the scheduler thread. No member is synchronized.
    /// </remarks>
    /// <seealso cref="Windows::Socket"/>
    /// <seealso cref="AsyncIo::AsyncSocket"/>
    export class Socket
    {
        Socket(const Socket&)            = delete;
        Socket& operator=(const Socket&) = delete;

        static constexpr int OptionLevel = SOL_SOCKET;

    public:
        /// <summary>Nothing to do on Linux; there for code that runs on Windows as well.</summary>
        /// <returns><see cref="Error::Success"/>.</returns>
        /// <seealso cref="Cleanup"/>
        static ErrorCode Startup() noexcept { return Error::Success; }

        /// <summary>Nothing to do on Linux; there for code that runs on Windows as well.</summary>
        /// <returns><see cref="Error::Success"/>.</returns>
        /// <seealso cref="Startup"/>
        static ErrorCode Cleanup() noexcept { return Error::Success; }

        /// <summary>Creates an object without a socket.</summary>
        Socket() noexcept = default;

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds none afterwards.</param>
        Socket(Socket&& other) noexcept : socket_(other.socket_) { other.socket_ = InvalidSocket; }

        /// <summary>Closes the socket if there is one (see <see cref="Close"/>).</summary>
        ~Socket() noexcept { Close(); }

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds none afterwards.</param>
        /// <returns>This object.</returns>
        /// <remarks>A socket this object already holds is closed first (see <see cref="Close"/>).</remarks>
        Socket& operator=(Socket&& other) noexcept
        {
            if (this != &other) {
                Close();
                socket_       = other.socket_;
                other.socket_ = InvalidSocket;
            }
            return *this;
        }

        /// <summary><c>true</c> if the object holds a socket.</summary>
        /// <returns><c>true</c> if the object holds a socket; <c>false</c> if it has none.</returns>
        bool IsValid() const noexcept { return socket_ != InvalidSocket; }


        /// <summary>Creates the socket (<c>socket</c>, with <c>SOCK_CLOEXEC</c>).</summary>
        /// <param name="addressFamily">The address family, e.g. <c>AF_INET</c> or <c>AF_INET6</c>.</param>
        /// <param name="type">The socket type, e.g. <c>SOCK_STREAM</c> or <c>SOCK_DGRAM</c>.</param>
        /// <param name="protocol">The protocol, e.g. <c>IPPROTO_TCP</c>; <c>0</c> lets the system choose.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>socket</c>.</returns>
        /// <remarks>The object must not hold a socket yet.</remarks>
        /// <seealso cref="Close"/>
        ErrorCode Create(int addressFamily, int type, int protocol) noexcept
        {
            CT_ASSERT(!IsValid());

            socket_ = ::socket(addressFamily, type | SOCK_CLOEXEC, protocol);
            return IsValid() ? Error::Success : Error::GetLastError();
        }

        /// <summary>Takes ownership of a descriptor, e.g. one an accept returned.</summary>
        /// <param name="socket">The descriptor; this object closes it from now on.</param>
        /// <remarks>The object must not hold a socket yet.</remarks>
        void Attach(int socket) noexcept
        {
            CT_ASSERT(!IsValid());

            socket_ = socket;
        }

        /// <summary>Closes the socket (<c>close</c>), if there is one; the object is then without socket.</summary>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>close</c>.</returns>
        /// <remarks>
        /// The descriptor is given up also when <c>close</c> reports an error, as Linux has released
        /// it then too. Operations in flight must have completed before (see
        /// <see cref="AsyncIo::AsyncSocket"/>).
        /// </remarks>
        /// <seealso cref="Create"/>
        ErrorCode Close() noexcept
        {
            if (!IsValid())
                return Error::Success;

            const bool closed = ::close(socket_) == 0;
            socket_           = InvalidSocket;
            return Error::GetError(closed);
        }

        /// <summary>Binds the socket to <paramref name="addr"/> (<c>bind</c>).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The local address and port; port <c>0</c> lets the system choose one.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>bind</c>.</returns>
        template <SockAddrType _SockAddr>
        ErrorCode Bind(const _SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::bind(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        }

        /// <summary>Connects to <paramref name="addr"/> (<c>connect</c>); blocks the calling thread.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The remote address and port.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>connect</c>.</returns>
        template <SockAddrType _SockAddr>
        ErrorCode Connect(const _SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::connect(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        }

        /// <summary>Gets the peer's address (<c>getpeername</c>).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="addr">Receives the peer's address; must be large enough for the socket's address family.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getpeername</c>.</returns>
        /// <seealso cref="GetSockName"/>
        template <SockAddrType _SockAddr>
        ErrorCode GetPeerName(_SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            socklen_t addrSize = sizeof(addr);
            return Error::GetError(::getpeername(socket_, reinterpret_cast<sockaddr*>(&addr), &addrSize) == 0);
        }

        /// <summary>Gets the local address (<c>getsockname</c>), e.g. the port chosen by <see cref="Bind"/> with port <c>0</c>.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="addr">Receives the local address; must be large enough for the socket's address family.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockname</c>.</returns>
        /// <seealso cref="GetPeerName"/>
        template <SockAddrType _SockAddr>
        ErrorCode GetSockName(_SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            socklen_t addrSize = sizeof(addr);
            return Error::GetError(::getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &addrSize) == 0);
        }

        /// <summary>Gets a socket option into a raw buffer (<c>getsockopt</c>).</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. Receives the option value.</param>
        /// <param name="valueLength">Required. In: the size of <paramref name="optionValue"/> in bytes; out: the size written.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        /// <seealso cref="SetOption"/>
        ErrorCode GetOption(int level, int optionName, void* optionValue, socklen_t* valueLength) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(optionValue);
            CT_ASSERT(valueLength);

            return Error::GetError(::getsockopt(socket_, level, optionName, optionValue, valueLength) == 0);
        }

        /// <summary>Gets a socket option of type <typeparamref name="T"/> (buffer size <c>sizeof(T)</c>).</summary>
        /// <typeparam name="T">The option's value type, e.g. <c>int</c> or <c>linger</c>.</typeparam>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. Receives the option value.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        template <typename T>
        ErrorCode GetOption(int level, int optionName, T* optionValue) noexcept
        {
            socklen_t length = sizeof(*optionValue);
            return GetOption(level, optionName, static_cast<void*>(optionValue), &length);
        }

        /// <summary>Gets an <c>int</c>-sized on/off socket option as <c>bool</c>.</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_KEEPALIVE</c>.</param>
        /// <param name="optionValue">Required. Receives <c>true</c> if the option is set (non-zero); unchanged on failure.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        template <>
        ErrorCode GetOption<bool>(int level, int optionName, bool* optionValue) noexcept
        {
            int        value = 0;
            const auto error = GetOption(level, optionName, &value);
            if (Error::Succeeded(error))
                *optionValue = value != 0;
            return error;
        }

        /// <summary>Sets a socket option from a raw buffer (<c>setsockopt</c>).</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. The new option value.</param>
        /// <param name="valueLength">The size of <paramref name="optionValue"/> in bytes.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        /// <seealso cref="GetOption"/>
        ErrorCode SetOption(int level, int optionName, const void* optionValue, socklen_t valueLength) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(optionValue);

            return Error::GetError(::setsockopt(socket_, level, optionName, optionValue, valueLength) == 0);
        }

        /// <summary>Sets a socket option from a value of type <typeparamref name="T"/>.</summary>
        /// <typeparam name="T">The option's value type, e.g. <c>int</c> or <c>linger</c>.</typeparam>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">The new option value.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        /// <remarks>The value is passed by address with size <c>sizeof(T)</c>.</remarks>
        template <typename T>
        ErrorCode SetOption(int level, int optionName, const T& optionValue) noexcept
        {
            return SetOption(level, optionName, static_cast<const void*>(&optionValue), sizeof(optionValue));
        }

        /// <summary>Sets an <c>int</c>-sized on/off socket option from <c>bool</c>.</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_KEEPALIVE</c>.</param>
        /// <param name="optionValue"><c>true</c> to enable the option, <c>false</c> to disable it.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        template <>
        ErrorCode SetOption<bool>(int level, int optionName, const bool& optionValue) noexcept
        {
            return SetOption(level, optionName, static_cast<int>(optionValue));
        }

        /// <summary>Disables sends and/or receives (<c>shutdown</c>).</summary>
        /// <param name="how"><c>SHUT_RD</c>, <c>SHUT_WR</c> or <c>SHUT_RDWR</c> (the values of <c>SD_RECEIVE</c>, <c>SD_SEND</c>, <c>SD_BOTH</c>).</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>shutdown</c>.</returns>
        ErrorCode Shutdown(int how) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::shutdown(socket_, how) == 0);
        }

        /// <summary>Puts the bound socket into listening state (<c>listen</c>).</summary>
        /// <param name="backlog">The maximum length of the queue of pending connections; <c>SOMAXCONN</c> for the system's limit.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>listen</c>.</returns>
        ErrorCode Listen(int backlog = SOMAXCONN) noexcept
        {
            CT_ASSERT(IsValid());

            return Error::GetError(::listen(socket_, backlog) == 0);
        }

        /// <summary>Gets whether the socket listens (<c>SO_ACCEPTCONN</c>).</summary>
        /// <param name="isListening">Required. Receives <c>true</c> if the socket is listening.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionIsListening(bool* isListening) noexcept { return GetOption(OptionLevel, SO_ACCEPTCONN, isListening); }

        /// <summary>Allows sending broadcasts (<c>SO_BROADCAST</c>).</summary>
        /// <param name="enable"><c>true</c> to allow broadcasts.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionBroadcast(bool enable) noexcept { return SetOption(OptionLevel, SO_BROADCAST, enable); }

        /// <summary>Gets whether broadcasts are allowed (<c>SO_BROADCAST</c>).</summary>
        /// <param name="enable">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionBroadcast(bool* enable) noexcept { return GetOption(OptionLevel, SO_BROADCAST, enable); }

        /// <summary>Sends without routing, to directly connected hosts only (<c>SO_DONTROUTE</c>).</summary>
        /// <param name="dontRoute"><c>true</c> to bypass routing.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionDontRoute(bool dontRoute) noexcept { return SetOption(OptionLevel, SO_DONTROUTE, dontRoute); }

        /// <summary>Gets whether routing is bypassed (<c>SO_DONTROUTE</c>).</summary>
        /// <param name="dontRoute">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionDontRoute(bool* dontRoute) noexcept { return GetOption(OptionLevel, SO_DONTROUTE, dontRoute); }

        /// <summary>Gets and clears the socket's pending error (<c>SO_ERROR</c>).</summary>
        /// <param name="error">Required. Receives the pending <c>errno</c>; <c>0</c> if none.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionError(ErrorCode* error) noexcept { return GetOption(OptionLevel, SO_ERROR, error); }

        /// <summary>Sends keep-alive probes on an idle connection (<c>SO_KEEPALIVE</c>).</summary>
        /// <param name="keepAlive"><c>true</c> to send keep-alives.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionKeepAlive(bool keepAlive) noexcept { return SetOption(OptionLevel, SO_KEEPALIVE, keepAlive); }

        /// <summary>Gets whether keep-alives are sent (<c>SO_KEEPALIVE</c>).</summary>
        /// <param name="keepAlive">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionKeepAlive(bool* keepAlive) noexcept { return GetOption(OptionLevel, SO_KEEPALIVE, keepAlive); }

        /// <summary>Sets how closing treats unsent data (<c>SO_LINGER</c>).</summary>
        /// <param name="l">On/off and the time in seconds to wait for unsent data.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionLinger(const linger& l) noexcept { return SetOption(OptionLevel, SO_LINGER, l); }

        /// <summary>Gets how closing treats unsent data (<c>SO_LINGER</c>).</summary>
        /// <param name="l">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionLinger(linger* l) noexcept { return GetOption(OptionLevel, SO_LINGER, l); }

        /// <summary>Receives out-of-band data in the normal stream (<c>SO_OOBINLINE</c>).</summary>
        /// <param name="inLine"><c>true</c> to receive it in line.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionOobInLine(bool inLine) noexcept { return SetOption(OptionLevel, SO_OOBINLINE, inLine); }

        /// <summary>Gets whether out-of-band data comes in line (<c>SO_OOBINLINE</c>).</summary>
        /// <param name="inLine">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionOobInLine(bool* inLine) noexcept { return GetOption(OptionLevel, SO_OOBINLINE, inLine); }

        /// <summary>Sets the receive buffer size (<c>SO_RCVBUF</c>).</summary>
        /// <param name="size">The size in bytes; Linux doubles it for its bookkeeping.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionReceiveBufferSize(std::uint32_t size) noexcept { return SetOption(OptionLevel, SO_RCVBUF, static_cast<int>(size)); }

        /// <summary>Gets the receive buffer size (<c>SO_RCVBUF</c>).</summary>
        /// <param name="size">Required. Receives the size in bytes.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionReceiveBufferSize(std::uint32_t* size) noexcept { return GetOption(OptionLevel, SO_RCVBUF, size); }

        /// <summary>Allows binding to an address in use (<c>SO_REUSEADDR</c>).</summary>
        /// <param name="reuseAddress"><c>true</c> to allow it.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionReuseAddress(bool reuseAddress) noexcept { return SetOption(OptionLevel, SO_REUSEADDR, reuseAddress); }

        /// <summary>Gets the reuse-address setting (<c>SO_REUSEADDR</c>).</summary>
        /// <param name="reuseAddress">Required. Receives the setting.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionReuseAddress(bool* reuseAddress) noexcept { return GetOption(OptionLevel, SO_REUSEADDR, reuseAddress); }

        /// <summary>Sets the send buffer size (<c>SO_SNDBUF</c>).</summary>
        /// <param name="size">The size in bytes; Linux doubles it for its bookkeeping.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>setsockopt</c>.</returns>
        ErrorCode SetOptionSendBufferSize(std::uint32_t size) noexcept { return SetOption(OptionLevel, SO_SNDBUF, static_cast<int>(size)); }

        /// <summary>Gets the send buffer size (<c>SO_SNDBUF</c>).</summary>
        /// <param name="size">Required. Receives the size in bytes.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionSendBufferSize(std::uint32_t* size) noexcept { return GetOption(OptionLevel, SO_SNDBUF, size); }

        /// <summary>Gets the socket type (<c>SO_TYPE</c>), e.g. <c>SOCK_STREAM</c>.</summary>
        /// <param name="type">Required. Receives the type.</param>
        /// <returns><see cref="Error::Success"/>, or the <c>errno</c> of <c>getsockopt</c>.</returns>
        ErrorCode GetOptionType(int* type) noexcept { return GetOption(OptionLevel, SO_TYPE, type); }

    protected:
        /// <summary>The descriptor, still owned by this object.</summary>
        /// <returns>The descriptor, or <c>-1</c> if none is held; do not close it.</returns>
        /// <remarks>Protected, as on Windows: the classes built on the socket use it.</remarks>
        int GetSocket() const noexcept { return socket_; }

    private:
        static constexpr int InvalidSocket = -1;

        int socket_ = InvalidSocket;
    };
} // namespace ThinkMeta::ConcurrentTasks::Posix
