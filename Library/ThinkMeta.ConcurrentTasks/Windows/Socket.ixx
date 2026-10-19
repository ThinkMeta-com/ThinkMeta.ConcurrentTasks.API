//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Socket: a thin owner of a Winsock SOCKET with Winsock initialization (Startup),
// the plain calls, typed socket options and the raw OVERLAPPED forms of the overlapped and
// extension-function operations. AsyncSocket builds the task-aware asynchronous I/O on top.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>

#include <Windows.h>

#include <MSWSock.h>
#include <ws2ipdef.h>

#include <concepts>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Windows.Socket;

export import ThinkMeta.ConcurrentTasks.Windows.WinsockError;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>The socket address structures accepted where an address is passed by type.</summary>
    /// <typeparam name="T">
    /// The type to check: <c>sockaddr</c>, <c>sockaddr_in</c>, <c>sockaddr_in6</c> or <c>sockaddr_storage</c>.
    /// </typeparam>
    /// <remarks>The address size passed to Winsock is <c>sizeof</c>(<typeparamref name="T"/>).</remarks>
    export template <typename T>
    concept SockAddrType = std::same_as<T, sockaddr> || std::same_as<T, sockaddr_in> || std::same_as<T, sockaddr_in6> || std::same_as<T, sockaddr_storage>;

    /// <summary>Owns one Winsock <c>SOCKET</c> and wraps the Winsock calls on it.</summary>
    /// <remarks>
    /// The socket is closed on destruction.
    ///
    /// <see cref="Socket::Startup"/> must have succeeded before any other use: it calls <c>WSAStartup</c>
    /// and loads the extension functions (<c>AcceptEx</c>, <c>ConnectEx</c>, <c>DisconnectEx</c>,
    /// <c>GetAcceptExSockaddrs</c>, <c>TransmitFile</c>, <c>TransmitPackets</c>, <c>WSARecvMsg</c>,
    /// <c>WSASendMsg</c>) that <see cref="Socket::AcceptAsync"/>, <see cref="Socket::ConnectAsync"/> etc.
    /// call through.
    ///
    /// Errors are reported as <see cref="WinsockErrorCode"/> (<see cref="WinsockError::Success"/> or a WSA*
    /// code from <c>WSAGetLastError</c>); nothing throws. The caller meets the preconditions (the object
    /// holds a socket, required pointers are given); Debug builds assert them. The plain calls
    /// (<see cref="Socket::Bind"/>, <see cref="Socket::Connect"/>, <see cref="Socket::Listen"/>,
    /// <see cref="Socket::Shutdown"/>, options ...) are ordinary Winsock calls: on a blocking socket a
    /// blocking call (e.g. Connect) blocks the OS thread, it does not suspend a task - use
    /// <see cref="AsyncIo::AsyncSocket"/>'s forms inside tasks.
    ///
    /// The XxxAsync(..., <c>OVERLAPPED</c>) forms only start an overlapped operation and return Success
    /// (completed synchronously), <see cref="WinsockError::Pending"/> or the WSA error; completing, waiting
    /// and lifetime of the OVERLAPPED and buffers are the caller's business (AsyncSocket ties them to the
    /// core's completion port). Several operations may be outstanding at once, each with its own
    /// OVERLAPPED. Calls that change the object (<see cref="Socket::Create"/>, <see cref="Socket::Close"/>,
    /// assignment) must not run concurrently with other calls on it.
    /// </remarks>
    /// <seealso cref="AsyncIo::AsyncSocket"/>
    /// <seealso cref="IpSocket"/>
    export class Socket
    {
        Socket(const Socket&)                  = delete;
        const Socket& operator=(const Socket&) = delete;

        static const int OptionLevel = SOL_SOCKET;

    protected:
        // The Winsock extension functions, loaded once per process by Startup (null before that, so
        // the operations that call through them need a successful Startup). Loaded through a TCP/IPv4
        // socket and used for all sockets.
        inline static LPFN_ACCEPTEX             AcceptEx             = nullptr;
        inline static LPFN_CONNECTEX            ConnectEx            = nullptr;
        inline static LPFN_DISCONNECTEX         DisconnectEx         = nullptr;
        inline static LPFN_GETACCEPTEXSOCKADDRS GetAcceptExSockaddrs = nullptr;
        inline static LPFN_TRANSMITFILE         TransmitFile         = nullptr;
        inline static LPFN_TRANSMITPACKETS      TransmitPackets      = nullptr;
        inline static LPFN_WSARECVMSG           WSARecvMsg           = nullptr;
        inline static LPFN_WSASENDMSG           WSASendMsg           = nullptr;

    public:
        /// <summary>Initializes Winsock 2.2 (<c>WSAStartup</c>) and loads the extension functions.</summary>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, the error of <c>WSAStartup</c>, or the error of creating the
        /// helper socket or loading an extension pointer (then Winsock has been cleaned up again).
        /// </returns>
        /// <remarks>
        /// Call once per process (or per <see cref="Cleanup"/>) before any socket is created, from any
        /// thread. Calls are counted by Winsock; balance each successful Startup with Cleanup.
        /// </remarks>
        /// <seealso cref="Cleanup"/>
        static WinsockErrorCode Startup() noexcept
        {
            WSADATA          wsaData;
            WinsockErrorCode error = WinsockErrorCode(WSAStartup(MAKEWORD(2, 2), &wsaData));

            if (WinsockError::Succeeded(error)) {
                {
                    Socket socket;
                    error = socket.Create(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);

                    GUID guidAcceptEx = WSAID_ACCEPTEX;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidAcceptEx, AcceptEx);
                    GUID guidConnectEx = WSAID_CONNECTEX;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidConnectEx, ConnectEx);
                    GUID guidDisconnectEx = WSAID_DISCONNECTEX;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidDisconnectEx, DisconnectEx);
                    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidGetAcceptExSockaddrs, GetAcceptExSockaddrs);
                    GUID guidTransmitFile = WSAID_TRANSMITFILE;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidTransmitFile, TransmitFile);
                    GUID guidTransmitPackets = WSAID_TRANSMITPACKETS;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidTransmitPackets, TransmitPackets);
                    GUID guidWSARecvMsg = WSAID_WSARECVMSG;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidWSARecvMsg, WSARecvMsg);
                    GUID guidWSASendMsg = WSAID_WSASENDMSG;
                    if (WinsockError::Succeeded(error))
                        error = socket.IoctlGetExtensionFunctionPointer(guidWSASendMsg, WSASendMsg);
                }

                // The helper socket is closed by now; take the WSAStartup back so a failed Startup
                // needs no Cleanup.
                if (WinsockError::Failed(error))
                    WSACleanup();
            }

            return error;
        }

        /// <summary>Takes back one <see cref="Startup"/> (<c>WSACleanup</c>); no socket may be in use any more.</summary>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>WSACleanup</c>.</returns>
        /// <seealso cref="Startup"/>
        static WinsockErrorCode Cleanup() noexcept { return WinsockError::GetError(WSACleanup()); }

        /// <summary>Creates an object without a socket.</summary>
        Socket() noexcept : socket_(InvalidSocket) {}

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
        Socket(Socket&& other) noexcept : socket_{other.socket_} { other.socket_ = InvalidSocket; }

        /// <summary>Closes the socket if there is one (see <see cref="Close"/>).</summary>
        ~Socket() noexcept { Close(); }

        /// <summary>Takes over <paramref name="other"/>'s socket; other is left without one.</summary>
        /// <param name="other">The object whose socket is taken over; it holds no socket afterwards.</param>
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

        /// <summary>Creates the socket (<c>WSASocket</c>; parameters as there).</summary>
        /// <param name="addressFamily">The address family, e.g. <c>AF_INET</c> or <c>AF_INET6</c>.</param>
        /// <param name="type">The socket type, e.g. <c>SOCK_STREAM</c> or <c>SOCK_DGRAM</c>.</param>
        /// <param name="protocol">The protocol, e.g. <c>IPPROTO_TCP</c> or <c>IPPROTO_UDP</c>; <c>0</c> lets the provider choose.</param>
        /// <param name="protocolInfo">
        /// Optional <c>WSAPROTOCOL_INFO</c> that selects the provider (and overrides the first three parameters
        /// when they are <c>FROM_PROTOCOL_INFO</c>); <c>nullptr</c> for the default.
        /// </param>
        /// <param name="g">The socket group (<c>0</c> for none, which is the usual value).</param>
        /// <param name="flags"><c>WSA_FLAG_*</c> flags, e.g. <c>WSA_FLAG_OVERLAPPED</c>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>WSASocket</c>.</returns>
        /// <remarks>
        /// The object must not hold a socket yet. Add <c>WSA_FLAG_OVERLAPPED</c> to
        /// <paramref name="flags"/> for overlapped operations; <see cref="AsyncIo::AsyncSocket::Create"/>
        /// does that and associates the socket with the core's completion port.
        /// </remarks>
        /// <seealso cref="Close"/>
        /// <seealso cref="Startup"/>
        WinsockErrorCode Create(int addressFamily, int type, int protocol, WSAPROTOCOL_INFO* protocolInfo = nullptr, GROUP g = 0, DWORD flags = 0) noexcept
        {
            CT_ASSERT(!IsValid());

            Close();
            socket_ = WSASocket(addressFamily, type, protocol, protocolInfo, g, flags);

            return IsValid() ? WinsockError::Success : WinsockError::GetLastError();
        }

        /// <summary>Closes the socket (<c>closesocket</c>), if there is one; the object is then without socket.</summary>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>closesocket</c>.</returns>
        /// <remarks>
        /// Closing aborts pending overlapped operations (they complete with
        /// <see cref="WinsockError::OperationAborted"/>); their <c>OVERLAPPED</c> and buffers must stay valid
        /// until those completions have been delivered.
        /// </remarks>
        /// <seealso cref="Create"/>
        /// <seealso cref="CancelAllAsyncRequests"/>
        WinsockErrorCode Close() noexcept
        {
            if (IsValid()) {
                // The socket is given up only when closesocket took it: with SO_LINGER and a
                // non-blocking socket it answers WSAEWOULDBLOCK and the socket stays open, so the
                // caller keeps the handle and may close it again (or wait and retry).
                if (closesocket(socket_) == 0) {
                    socket_ = InvalidSocket;
                    return WinsockError::Success;
                }

                const auto error = WinsockError::GetLastError();
                CT_ASSERT(error == WSAEWOULDBLOCK);
                return error;
            }

            return WinsockError::Success;
        }

        /// <summary>Starts an <c>AcceptEx</c> on this listening socket (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="acceptSocket">Takes the accepted connection. A created, unbound and unconnected socket.</param>
        /// <param name="outputBuffer">
        /// Receives the first <paramref name="receiveDataLength"/> bytes of data followed by the local and remote
        /// address (parse with <c>GetAcceptExSockaddrs</c>). Must stay valid until completion.
        /// </param>
        /// <param name="receiveDataLength">
        /// Bytes of initial data to wait for; <c>0</c> completes as soon as a connection arrives, otherwise only
        /// after that data (or the connection's end).
        /// </param>
        /// <param name="localAddressLength">Space reserved for the local address: at least the address size + 16.</param>
        /// <param name="remoteAddressLength">Space reserved for the remote address: at least the address size + 16.</param>
        /// <param name="bytesReceived">Required. Receives the byte count on synchronous completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>
        /// See also <see cref="AsyncIo::AsyncSocket::AcceptAsync"/>. After the accept has completed, call
        /// acceptSocket.<see cref="SetOptionUpdateAcceptContext"/>(*this).
        /// </remarks>
        /// <seealso cref="Listen"/>
        WinsockErrorCode AcceptAsync(Socket& acceptSocket, void* outputBuffer, DWORD receiveDataLength, DWORD localAddressLength, DWORD remoteAddressLength,
                                     DWORD* bytesReceived, OVERLAPPED& overlapped) const noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(bytesReceived);
            CT_ASSERT(acceptSocket.IsValid());

            BOOL success
                = AcceptEx(socket_, acceptSocket.socket_, outputBuffer, receiveDataLength, localAddressLength, remoteAddressLength, bytesReceived, &overlapped);

            return WinsockError::GetAsyncError(success);
        }

        /// <summary>Binds the socket to <paramref name="addr"/> (<c>bind</c>).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The local address and port; port <c>0</c> lets the system choose one.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>bind</c>.</returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode Bind(const _SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(bind(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
        }

        /// <summary>Connects to <paramref name="addr"/> (<c>connect</c>); blocks the calling thread on a blocking socket.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The remote address and port.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>connect</c>.</returns>
        /// <seealso cref="ConnectAsync"/>
        template <SockAddrType _SockAddr>
        WinsockErrorCode Connect(const _SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(connect(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
        }

        /// <summary>Starts a <c>ConnectEx</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The remote address and port.</param>
        /// <param name="sendBuffer">Optional data sent once connected; <paramref name="bytesSent"/> is required with it.</param>
        /// <param name="sendDataLength">The size of <paramref name="sendBuffer"/> in bytes; <c>0</c> without send data.</param>
        /// <param name="bytesSent">Required when <paramref name="sendBuffer"/> is given; receives the bytes sent on synchronous completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>
        /// The socket must be bound first (<see cref="Bind"/>). After the connect has completed, call
        /// <see cref="SetOptionUpdateConnectContext"/>.
        /// </remarks>
        /// <seealso cref="AsyncIo::AsyncSocket::ConnectAsync"/>
        template <SockAddrType _SockAddr>
        WinsockErrorCode ConnectAsync(const _SockAddr& addr, void* sendBuffer, DWORD sendDataLength, DWORD* bytesSent, OVERLAPPED& overlapped) noexcept
        {
            return ConnectAsync(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), sendBuffer, sendDataLength, bytesSent, overlapped);
        }

        /// <summary>Starts a <c>ConnectEx</c> to an address of a given size (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="addr">
        /// Required; e.g. a <c>sockaddr_in</c> or <c>sockaddr_in6</c> (or a <c>sockaddr_storage</c> holding one).
        /// </param>
        /// <param name="addrSize">The size of the actual address at <paramref name="addr"/>, e.g. <c>sizeof(sockaddr_in)</c>.</param>
        /// <param name="sendBuffer">Optional data sent once connected; <paramref name="bytesSent"/> is required with it.</param>
        /// <param name="sendDataLength">The size of <paramref name="sendBuffer"/> in bytes; <c>0</c> without send data.</param>
        /// <param name="bytesSent">Required when <paramref name="sendBuffer"/> is given; receives the bytes sent on synchronous completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>
        /// The socket must be bound first (<see cref="Bind"/>). After the connect has completed, call
        /// <see cref="SetOptionUpdateConnectContext"/>.
        /// </remarks>
        /// <seealso cref="AsyncIo::AsyncSocket::ConnectAsync"/>
        WinsockErrorCode ConnectAsync(const sockaddr* addr, int addrSize, void* sendBuffer, DWORD sendDataLength, DWORD* bytesSent,
                                      OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(addr);
            CT_ASSERT(!sendBuffer || sendBuffer && bytesSent);

            auto success = ConnectEx(socket_, addr, addrSize, sendBuffer, sendDataLength, bytesSent, &overlapped);

            return WinsockError::GetAsyncError(success);
        }

        /// <summary>Connects with <c>WSAConnect</c> (caller/callee data, QoS).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="addr">The remote address and port.</param>
        /// <param name="callerData">Optional user data sent to the peer during connection setup; <c>nullptr</c> for none.</param>
        /// <param name="calleeData">Optional; receives user data sent back by the peer during connection setup.</param>
        /// <param name="sQos">Optional flow specifications (<c>QOS</c>) for the socket; <c>nullptr</c> for none.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>WSAConnect</c>.</returns>
        /// <remarks>Blocks the calling thread on a blocking socket.</remarks>
        template <SockAddrType _SockAddr>
        WinsockErrorCode Connect(const _SockAddr& addr, WSABUF* callerData, WSABUF* calleeData, QOS* sQos) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(WSAConnect(socket_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), callerData, calleeData, sQos, nullptr));
        }

        /// <summary>Starts a <c>DisconnectEx</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <param name="flags">
        /// <c>0</c> or <c>TF_REUSE_SOCKET</c> (the socket can then be passed to <see cref="AcceptAsync"/>/
        /// <see cref="ConnectAsync"/> again).
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode DisconnectAsync(OVERLAPPED& overlapped, DWORD flags = 0) noexcept
        {
            CT_ASSERT(IsValid());

            BOOL success = DisconnectEx(socket_, &overlapped, flags, 0);
            return WinsockError::GetAsyncError(success);
        }

        /// <summary>Gets the peer's address (<c>getpeername</c>).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="addr">Receives the peer's address; must be large enough for the socket's address family.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getpeername</c>.</returns>
        /// <seealso cref="GetSockName"/>
        template <SockAddrType _SockAddr>
        WinsockErrorCode GetPeerName(_SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            int addrSize = sizeof(addr);
            return WinsockError::GetError(getpeername(socket_, reinterpret_cast<sockaddr*>(&addr), &addrSize));
        }

        /// <summary>Gets the local address (<c>getsockname</c>), e.g. the port chosen by <see cref="Bind"/> with port <c>0</c>.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="addr">Receives the local address; must be large enough for the socket's address family.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockname</c>.</returns>
        /// <seealso cref="GetPeerName"/>
        template <SockAddrType _SockAddr>
        WinsockErrorCode GetSockName(_SockAddr& addr) noexcept
        {
            CT_ASSERT(IsValid());

            int addrSize = sizeof(addr);
            return WinsockError::GetError(getsockname(socket_, reinterpret_cast<sockaddr*>(&addr), &addrSize));
        }

        /// <summary>Gets a socket option into a raw buffer (<c>getsockopt</c>).</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. Receives the option value.</param>
        /// <param name="valueLength">Required. In: the size of <paramref name="optionValue"/> in bytes; out: the size written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        /// <seealso cref="SetOption"/>
        WinsockErrorCode GetOption(int level, int optionName, char* optionValue, int* valueLength) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(optionValue);
            CT_ASSERT(valueLength);

            return WinsockError::GetError(getsockopt(socket_, level, optionName, optionValue, valueLength));
        }

        /// <summary>Gets a socket option of type <typeparamref name="T"/> (buffer size <c>sizeof(T)</c>).</summary>
        /// <typeparam name="T">The option's value type, e.g. <c>DWORD</c> or <c>linger</c>.</typeparam>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. Receives the option value.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        template <typename T>
        WinsockErrorCode GetOption(int level, int optionName, T* optionValue) noexcept
        {
            int length = sizeof(*optionValue);
            return GetOption(level, optionName, reinterpret_cast<char*>(optionValue), &length);
        }

        /// <summary>Gets a <c>BOOL</c>-sized socket option as <c>bool</c>.</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_KEEPALIVE</c>.</param>
        /// <param name="optionValue">Required. Receives <c>true</c> if the option is set (non-zero); unchanged on failure.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        template <>
        WinsockErrorCode GetOption<bool>(int level, int optionName, bool* optionValue) noexcept
        {
            DWORD            dwOptionValue;
            WinsockErrorCode error = GetOption(level, optionName, &dwOptionValue);
            if (WinsockError::Succeeded(error))
                *optionValue = (dwOptionValue != 0);
            return error;
        }

        /// <summary>Sets a socket option from a raw buffer (<c>setsockopt</c>).</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">Required. The new option value.</param>
        /// <param name="valueLength">The size of <paramref name="optionValue"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <seealso cref="GetOption"/>
        WinsockErrorCode SetOption(int level, int optionName, const char* optionValue, int valueLength) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(optionValue);

            return WinsockError::GetError(setsockopt(socket_, level, optionName, optionValue, valueLength));
        }

        /// <summary>Sets a socket option that takes no value (e.g. <c>SO_UPDATE_CONNECT_CONTEXT</c>).</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>.</param>
        /// <param name="option">The option at that level.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOption(int level, int option) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(setsockopt(socket_, level, option, nullptr, 0));
        }

        /// <summary>Sets a socket option from a value of type <typeparamref name="T"/>.</summary>
        /// <typeparam name="T">The option's value type, e.g. <c>DWORD</c> or <c>linger</c>.</typeparam>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_RCVBUF</c>.</param>
        /// <param name="optionValue">The new option value.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>The value is passed by address with size <c>sizeof(T)</c>.</remarks>
        template <typename T>
        WinsockErrorCode SetOption(int level, int optionName, const T& optionValue) noexcept
        {
            return SetOption(level, optionName, reinterpret_cast<const char*>(&optionValue), sizeof(optionValue));
        }

        /// <summary>Sets a <c>BOOL</c>-sized socket option from <c>bool</c>.</summary>
        /// <param name="level">The option level, e.g. <c>SOL_SOCKET</c>, <c>IPPROTO_IP</c>, <c>IPPROTO_IPV6</c>, <c>IPPROTO_TCP</c>.</param>
        /// <param name="optionName">The option at that level, e.g. <c>SO_KEEPALIVE</c>.</param>
        /// <param name="optionValue"><c>true</c> to enable the option, <c>false</c> to disable it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        template <>
        WinsockErrorCode SetOption<bool>(int level, int optionName, const bool& optionValue) noexcept
        {
            return SetOption(level, optionName, static_cast<DWORD>(optionValue));
        }

        /// <summary>Disables sends and/or receives (<c>shutdown</c>).</summary>
        /// <param name="how"><c>SD_RECEIVE</c>, <c>SD_SEND</c> or <c>SD_BOTH</c>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>shutdown</c>.</returns>
        /// <seealso cref="DisconnectAsync"/>
        WinsockErrorCode Shutdown(int how) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(shutdown(socket_, how));
        }

// Typed SOL_SOCKET options; each is a setsockopt/getsockopt with the named option. See the
// Winsock documentation of the option for meaning and validity.
#pragma region Socket options

        /// <summary>Sets the service provider's configuration data (<c>PVD_CONFIG</c>).</summary>
        /// <param name="configuration">Required. The provider-specific configuration data.</param>
        /// <param name="length">The size of <paramref name="configuration"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionProviderConfiguration(const char* configuration, int length) noexcept
        {
            return SetOption(OptionLevel, PVD_CONFIG, configuration, length);
        }

        /// <summary>Gets the service provider's configuration data (<c>PVD_CONFIG</c>).</summary>
        /// <param name="configuration">Required. Receives the provider-specific configuration data.</param>
        /// <param name="length">Required. In: the size of <paramref name="configuration"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionProviderConfiguration(char* configuration, int* length) noexcept
        {
            return GetOption(OptionLevel, PVD_CONFIG, configuration, length);
        }

        /// <summary>Gets whether the socket is listening (<c>SO_ACCEPTCONN</c>).</summary>
        /// <param name="isListening">Required. Receives <c>true</c> if the socket is in listening state.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionIsListening(bool* isListening) noexcept
        {
            return GetOption(OptionLevel, SO_ACCEPTCONN, isListening);
        }

        /// <summary>Enables sending broadcast messages (<c>SO_BROADCAST</c>).</summary>
        /// <param name="enable"><c>true</c> to allow sending to broadcast addresses, <c>false</c> to forbid it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionBroadcast(bool enable) noexcept
        {
            return SetOption(OptionLevel, SO_BROADCAST, enable);
        }

        /// <summary>Gets whether broadcast is enabled (<c>SO_BROADCAST</c>).</summary>
        /// <param name="enable">Required. Receives <c>true</c> if sending broadcast messages is allowed.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionBroadcast(bool* enable) noexcept
        {
            return GetOption(OptionLevel, SO_BROADCAST, enable);
        }

        /// <summary>Gets the local and remote address (<c>SO_BSP_STATE</c>).</summary>
        /// <param name="buffer">Receives a <c>CSADDR_INFO</c> followed by the addresses it points to.</param>
        /// <param name="length">
        /// In: the size of <paramref name="buffer"/>; out: the bytes written. A <c>CSADDR_INFO</c> plus
        /// two <c>SOCKADDR_STORAGE</c> is always enough.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionBspState(void* buffer, int* length) noexcept
        {
            return GetOption(OptionLevel, SO_BSP_STATE, static_cast<char*>(buffer), length);
        }

        /// <summary>Enables conditional accept (<c>WSAAccept</c> condition function) (<c>SO_CONDITIONAL_ACCEPT</c>).</summary>
        /// <param name="accept"><c>true</c> to enable conditional accept, <c>false</c> to disable it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionConditionalAccept(bool accept) noexcept
        {
            return SetOption(OptionLevel, SO_CONDITIONAL_ACCEPT, accept);
        }

        /// <summary>Gets whether conditional accept is enabled (<c>SO_CONDITIONAL_ACCEPT</c>).</summary>
        /// <param name="accept">Required. Receives <c>true</c> if conditional accept is enabled.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionConditionalAccept(bool* accept) noexcept
        {
            return GetOption(OptionLevel, SO_CONDITIONAL_ACCEPT, accept);
        }

        /// <summary>Sets the connect data (protocols that support it) (<c>SO_CONNDATA</c>).</summary>
        /// <param name="data">Required. The data to send with the connection request.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionConnectionData(const char* data, int length) noexcept
        {
            return SetOption(OptionLevel, SO_CONNDATA, data, length);
        }

        /// <summary>Gets the connect data (<c>SO_CONNDATA</c>).</summary>
        /// <param name="data">Required. Receives the connect data.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionConnectionData(char* data, int* length) noexcept
        {
            return GetOption(OptionLevel, SO_CONNDATA, data, length);
        }

        /// <summary>Sets the length of the connect data buffer (<c>SO_CONNDATALEN</c>).</summary>
        /// <param name="length">The buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionConnectionDataLength(ULONG length) noexcept
        {
            return SetOption(OptionLevel, SO_CONNDATALEN, length);
        }

        /// <summary>Gets the seconds the socket has been connected (<c>SO_CONNECT_TIME</c>).</summary>
        /// <param name="connectTime">Required. Receives the connection time in seconds.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        /// <remarks><c>0xFFFFFFFF</c> if not connected.</remarks>
        WinsockErrorCode GetOptionConnectionTime(ULONG* connectTime) noexcept
        {
            return GetOption(OptionLevel, SO_CONNECT_TIME, connectTime);
        }

        /// <summary>Sets the connect options data (<c>SO_CONNOPT</c>).</summary>
        /// <param name="data">Required. The connect options data.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionConnectOptionData(const char* data, int length) noexcept
        {
            return SetOption(OptionLevel, SO_CONNOPT, data, length);
        }

        /// <summary>Gets the connect options data (<c>SO_CONNOPT</c>).</summary>
        /// <param name="data">Required. Receives the connect options data.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionConnectOptionData(char* data, int* length) noexcept
        {
            return GetOption(OptionLevel, SO_CONNOPT, data, length);
        }

        /// <summary>Sets the length of the connect options buffer (<c>SO_CONNOPTLEN</c>).</summary>
        /// <param name="length">The buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionConnectOptionDataLength(ULONG length) noexcept
        {
            return SetOption(OptionLevel, SO_CONNOPTLEN, length);
        }

        /// <summary>Sets the disconnect data (<c>SO_DISCDATA</c>).</summary>
        /// <param name="data">Required. The data to send with the disconnect.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDisconnectionData(const char* data, int length) noexcept
        {
            return SetOption(OptionLevel, SO_DISCDATA, data, length);
        }

        /// <summary>Gets the disconnect data (<c>SO_DISCDATA</c>).</summary>
        /// <param name="data">Required. Receives the disconnect data.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDisconnectionData(char* data, int* length) noexcept
        {
            return GetOption(OptionLevel, SO_DISCDATA, data, length);
        }

        /// <summary>Sets the length of the disconnect data buffer (<c>SO_DISCDATALEN</c>).</summary>
        /// <param name="length">The buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDisconnectionDataLength(ULONG length) noexcept
        {
            return SetOption(OptionLevel, SO_DISCDATALEN, length);
        }

        /// <summary>Sets the disconnect options data (<c>SO_DISCOPT</c>).</summary>
        /// <param name="data">Required. The disconnect options data.</param>
        /// <param name="length">The size of <paramref name="data"/> in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDisconnectOptionData(const char* data, int length) noexcept { return SetOption(OptionLevel, SO_DISCOPT, data, length); }

        /// <summary>Gets the disconnect options data (<c>SO_DISCOPT</c>).</summary>
        /// <param name="data">Required. Receives the disconnect options data.</param>
        /// <param name="length">Required. In: the size of <paramref name="data"/> in bytes; out: the bytes written.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDisconnectOptionData(char* data, int* length) noexcept
        {
            return GetOption(OptionLevel, SO_DISCOPT, data, length);
        }

        /// <summary>Sets the length of the disconnect options buffer (<c>SO_DISCOPTLEN</c>).</summary>
        /// <param name="length">The buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDisconnectOptionDataLength(ULONG length) noexcept
        {
            return SetOption(OptionLevel, SO_DISCOPTLEN, length);
        }

        /// <summary>Enables debug output (<c>SO_DEBUG</c>).</summary>
        /// <param name="debug"><c>true</c> to enable debug output, <c>false</c> to disable it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDebug(bool debug) noexcept
        {
            return SetOption(OptionLevel, SO_DEBUG, debug);
        }

        /// <summary>Gets whether debug output is enabled (<c>SO_DEBUG</c>).</summary>
        /// <param name="debug">Required. Receives <c>true</c> if debug output is enabled.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDebug(bool* debug) noexcept
        {
            return GetOption(OptionLevel, SO_DEBUG, debug);
        }

        /// <summary>Sets whether close returns without lingering for unsent data (<c>SO_DONTLINGER</c>).</summary>
        /// <param name="dontLinger"><c>true</c>: close does not wait for unsent data; <c>false</c>: the <c>SO_LINGER</c> setting applies.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDontLinger(bool dontLinger) noexcept
        {
            return SetOption(OptionLevel, SO_DONTLINGER, dontLinger);
        }

        /// <summary>Gets the don't-linger setting (<c>SO_DONTLINGER</c>).</summary>
        /// <param name="dontLinger">Required. Receives <c>true</c> if close does not linger for unsent data.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDontLinger(bool* dontLinger) noexcept
        {
            return GetOption(OptionLevel, SO_DONTLINGER, dontLinger);
        }

        /// <summary>Sets whether outgoing data bypasses routing (<c>SO_DONTROUTE</c>).</summary>
        /// <param name="dontRoute"><c>true</c> to send directly to the interface without routing, <c>false</c> for normal routing.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionDontRoute(bool dontRoute) noexcept
        {
            return SetOption(OptionLevel, SO_DONTROUTE, dontRoute);
        }

        /// <summary>Gets the don't-route setting (<c>SO_DONTROUTE</c>).</summary>
        /// <param name="dontRoute">Required. Receives <c>true</c> if outgoing data bypasses routing.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionDontRoute(bool* dontRoute) noexcept
        {
            return GetOption(OptionLevel, SO_DONTROUTE, dontRoute);
        }

        /// <summary>Gets and clears the socket's pending error (<c>SO_ERROR</c>).</summary>
        /// <param name="error">Required. Receives the pending WSA error code, <c>0</c> if there is none.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionError(DWORD* error) noexcept
        {
            return GetOption(OptionLevel, SO_ERROR, error);
        }

        /// <summary>Prevents other sockets from binding to the same address (<c>SO_EXCLUSIVEADDRUSE</c>).</summary>
        /// <param name="exclusiveAddressUse"><c>true</c> for exclusive use of the address; set before <see cref="Bind"/>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionExclusiveAddressUse(bool exclusiveAddressUse) noexcept
        {
            return SetOption(OptionLevel, SO_EXCLUSIVEADDRUSE, exclusiveAddressUse);
        }

        /// <summary>Gets the exclusive-address setting (<c>SO_EXCLUSIVEADDRUSE</c>).</summary>
        /// <param name="exclusiveAddressUse">Required. Receives <c>true</c> if the address is used exclusively.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionExclusiveAddressUse(bool* exclusiveAddressUse) noexcept
        {
            return GetOption(OptionLevel, SO_EXCLUSIVEADDRUSE, exclusiveAddressUse);
        }

        /// <summary>Gets the socket group id (<c>SO_GROUP_ID</c>).</summary>
        /// <param name="groupId">Required. Receives the id of the group the socket belongs to (<c>0</c> for none).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionGroupId(unsigned int* groupId) noexcept
        {
            return GetOption(OptionLevel, SO_GROUP_ID, groupId);
        }

        /// <summary>Sets the priority within the socket group (<c>SO_GROUP_PRIORITY</c>).</summary>
        /// <param name="priority">The relative priority; <c>0</c> is the highest.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionGroupPriority(int priority) noexcept
        {
            return SetOption(OptionLevel, SO_GROUP_PRIORITY, priority);
        }

        /// <summary>Gets the priority within the socket group (<c>SO_GROUP_PRIORITY</c>).</summary>
        /// <param name="priority">Required. Receives the relative priority; <c>0</c> is the highest.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionGroupPriority(int* priority) noexcept
        {
            return GetOption(OptionLevel, SO_GROUP_PRIORITY, priority);
        }

        /// <summary>Enables keep-alive packets (<c>SO_KEEPALIVE</c>).</summary>
        /// <param name="keepAlive"><c>true</c> to send keep-alive packets, <c>false</c> to stop them.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionKeepAlive(bool keepAlive) noexcept
        {
            return SetOption(OptionLevel, SO_KEEPALIVE, keepAlive);
        }

        /// <summary>Gets whether keep-alive is enabled (<c>SO_KEEPALIVE</c>).</summary>
        /// <param name="keepAlive">Required. Receives <c>true</c> if keep-alive packets are sent.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionKeepAlive(bool* keepAlive) noexcept
        {
            return GetOption(OptionLevel, SO_KEEPALIVE, keepAlive);
        }

        /// <summary>Sets the linger behaviour of close (<c>SO_LINGER</c>).</summary>
        /// <param name="l">
        /// <c>l_onoff</c> non-zero to linger on close; <c>l_linger</c> is the timeout in seconds (<c>0</c> resets
        /// the connection on close).
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionLinger(const linger& l) noexcept
        {
            return SetOption(OptionLevel, SO_LINGER, l);
        }

        /// <summary>Gets the linger behaviour of close (<c>SO_LINGER</c>).</summary>
        /// <param name="l">Required. Receives the linger setting.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionLinger(linger* l) noexcept
        {
            return GetOption(OptionLevel, SO_LINGER, l);
        }

        /// <summary>Gets the maximum message size of a message-oriented socket (<c>SO_MAX_MSG_SIZE</c>).</summary>
        /// <param name="size">Required. Receives the maximum message size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMaxMessageSize(ULONG* size) noexcept
        {
            return GetOption(OptionLevel, SO_MAX_MSG_SIZE, size);
        }

        /// <summary>Gets the maximum datagram size (<c>SO_MAXDG</c>).</summary>
        /// <param name="size">Required. Receives the maximum datagram size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMaxDatagramSize(ULONG* size) noexcept
        {
            return GetOption(OptionLevel, SO_MAXDG, size);
        }

        /// <summary>Gets the maximum datagram size for a destination (<c>SO_MAXPATHDG</c>).</summary>
        /// <param name="size">Required. Receives the maximum datagram size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionMaxPathDatagramSize(ULONG* size) noexcept
        {
            return GetOption(OptionLevel, SO_MAXPATHDG, size);
        }

        /// <summary>Sets whether out-of-band data is received inline (<c>SO_OOBINLINE</c>).</summary>
        /// <param name="inLine"><c>true</c> to receive out-of-band data in the normal data stream.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionOobInLine(bool inLine) noexcept
        {
            return SetOption(OptionLevel, SO_OOBINLINE, inLine);
        }

        /// <summary>Gets the out-of-band-inline setting (<c>SO_OOBINLINE</c>).</summary>
        /// <param name="inLine">Required. Receives <c>true</c> if out-of-band data is received inline.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionOobInLine(bool* inLine) noexcept
        {
            return GetOption(OptionLevel, SO_OOBINLINE, inLine);
        }

        /// <summary>Sets the open type of sockets later created by this thread (<c>SO_OPENTYPE</c>).</summary>
        /// <param name="type"><c>0</c> (overlapped) or <c>SO_SYNCHRONOUS_ALERT</c> / <c>SO_SYNCHRONOUS_NONALERT</c>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionOpenType(DWORD type) noexcept
        {
            return SetOption(OptionLevel, SO_OPENTYPE, type);
        }

        /// <summary>Gets the open type (<c>SO_OPENTYPE</c>).</summary>
        /// <param name="type">Required. Receives the open type (<c>0</c>, <c>SO_SYNCHRONOUS_ALERT</c> or <c>SO_SYNCHRONOUS_NONALERT</c>).</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionOpenType(DWORD* type) noexcept
        {
            return GetOption(OptionLevel, SO_OPENTYPE, type);
        }

        /// <summary>Enables local port scalability (<c>SO_PORT_SCALABILITY</c>).</summary>
        /// <param name="enable"><c>true</c> to enable port scalability, <c>false</c> to disable it.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionPortScalability(bool enable) noexcept
        {
            return SetOption(OptionLevel, SO_PORT_SCALABILITY, enable);
        }

        /// <summary>Gets the port scalability setting (<c>SO_PORT_SCALABILITY</c>).</summary>
        /// <param name="enable">Required. Receives <c>true</c> if port scalability is enabled.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionPortScalability(bool* enable) noexcept
        {
            return GetOption(OptionLevel, SO_PORT_SCALABILITY, enable);
        }

        /// <summary>Gets the protocol information (ANSI) (<c>SO_PROTOCOL_INFOA</c>).</summary>
        /// <param name="protocolInfo">Required. Receives the protocol information of the socket's provider.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionProtocolInfo(WSAPROTOCOL_INFOA* protocolInfo) noexcept
        {
            return GetOption(OptionLevel, SO_PROTOCOL_INFOA, protocolInfo);
        }

        /// <summary>Gets the protocol information (Unicode) (<c>SO_PROTOCOL_INFOW</c>).</summary>
        /// <param name="protocolInfo">Required. Receives the protocol information of the socket's provider.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionProtocolInfo(WSAPROTOCOL_INFOW* protocolInfo) noexcept
        {
            return GetOption(OptionLevel, SO_PROTOCOL_INFOW, protocolInfo);
        }

        /// <summary>Sets the receive buffer size (<c>SO_RCVBUF</c>).</summary>
        /// <param name="size">The buffer size in bytes; <c>0</c> disables the provider's receive buffering.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionReceiveBufferSize(DWORD size) noexcept
        {
            return SetOption(OptionLevel, SO_RCVBUF, size);
        }

        /// <summary>Gets the receive buffer size (<c>SO_RCVBUF</c>).</summary>
        /// <param name="size">Required. Receives the buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionReceiveBufferSize(DWORD* size) noexcept
        {
            return GetOption(OptionLevel, SO_RCVBUF, size);
        }

        /// <summary>Sets the receive timeout in milliseconds (<c>SO_RCVTIMEO</c>).</summary>
        /// <param name="timeout">The timeout in milliseconds; <c>0</c> waits without limit.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>
        /// Applies to blocking calls only; overlapped operations ignore it - use the timeout of
        /// <see cref="AsyncIo::AsyncSocket"/>'s waiting forms.
        /// </remarks>
        WinsockErrorCode SetOptionReceiveTimeout(DWORD timeout) noexcept
        {
            return SetOption(OptionLevel, SO_RCVTIMEO, timeout);
        }

        /// <summary>Gets the receive timeout in milliseconds (<c>SO_RCVTIMEO</c>).</summary>
        /// <param name="timeout">Required. Receives the timeout in milliseconds; <c>0</c> means no limit.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionReceiveTimeout(DWORD* timeout) noexcept
        {
            return GetOption(OptionLevel, SO_RCVTIMEO, timeout);
        }

        /// <summary>Allows binding to an address in use (<c>SO_REUSEADDR</c>).</summary>
        /// <param name="reuseAddress"><c>true</c> to allow binding to an address already in use; set before <see cref="Bind"/>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionReuseAddress(bool reuseAddress) noexcept
        {
            return SetOption(OptionLevel, SO_REUSEADDR, reuseAddress);
        }

        /// <summary>Gets the reuse-address setting (<c>SO_REUSEADDR</c>).</summary>
        /// <param name="reuseAddress">Required. Receives <c>true</c> if binding to an address in use is allowed.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionReuseAddress(bool* reuseAddress) noexcept
        {
            return GetOption(OptionLevel, SO_REUSEADDR, reuseAddress);
        }

        /// <summary>Sets the send buffer size (<c>SO_SNDBUF</c>).</summary>
        /// <param name="size">The buffer size in bytes; <c>0</c> disables the provider's send buffering.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionSendBufferSize(DWORD size) noexcept
        {
            return SetOption(OptionLevel, SO_SNDBUF, size);
        }

        /// <summary>Gets the send buffer size (<c>SO_SNDBUF</c>).</summary>
        /// <param name="size">Required. Receives the buffer size in bytes.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionSendBufferSize(DWORD* size) noexcept
        {
            return GetOption(OptionLevel, SO_SNDBUF, size);
        }

        /// <summary>Sets the send timeout in milliseconds (<c>SO_SNDTIMEO</c>).</summary>
        /// <param name="timeout">The timeout in milliseconds; <c>0</c> waits without limit.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>Applies to blocking calls only; overlapped operations ignore it.</remarks>
        WinsockErrorCode SetOptionSendTimeout(DWORD timeout) noexcept
        {
            return SetOption(OptionLevel, SO_SNDTIMEO, timeout);
        }

        /// <summary>Gets the send timeout in milliseconds (<c>SO_SNDTIMEO</c>).</summary>
        /// <param name="timeout">Required. Receives the timeout in milliseconds; <c>0</c> means no limit.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionSendTimeout(DWORD* timeout) noexcept
        {
            return GetOption(OptionLevel, SO_SNDTIMEO, timeout);
        }

        /// <summary>Gets the socket type (<c>SOCK_STREAM</c>, <c>SOCK_DGRAM</c>, ...) (<c>SO_TYPE</c>).</summary>
        /// <param name="socketType">Required. Receives the socket type.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionType(DWORD* socketType) noexcept
        {
            return GetOption(OptionLevel, SO_TYPE, socketType);
        }

        /// <summary>Sets <c>SO_UPDATE_ACCEPT_CONTEXT</c> on an accepted socket.</summary>
        /// <param name="listenSocket">The listening socket the <c>AcceptEx</c> ran on.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>
        /// Call on the accepted socket after <see cref="AcceptAsync"/> has completed; <c>getpeername</c>,
        /// <c>shutdown</c> etc. work on it only after that. <see cref="AsyncIo::AsyncTcpSocket::Accept"/> does it.
        /// </remarks>
        /// <seealso cref="SetOptionUpdateConnectContext"/>
        WinsockErrorCode SetOptionUpdateAcceptContext(const Socket& listenSocket) noexcept
        {
            return SetOption(OptionLevel, SO_UPDATE_ACCEPT_CONTEXT, listenSocket.socket_);
        }

        /// <summary>Sets <c>SO_UPDATE_CONNECT_CONTEXT</c> on a socket connected by <c>ConnectEx</c>.</summary>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        /// <remarks>
        /// Call after <see cref="ConnectAsync"/> has completed; <c>getpeername</c>, <c>shutdown</c> etc. work
        /// on the socket only after that. <see cref="AsyncIo::AsyncTcpSocket::Connect"/> does it.
        /// </remarks>
        /// <seealso cref="SetOptionUpdateAcceptContext"/>
        WinsockErrorCode SetOptionUpdateConnectContext() noexcept { return SetOption(OptionLevel, SO_UPDATE_CONNECT_CONTEXT); }

        /// <summary>Sets whether the loopback is used (<c>SO_USELOOPBACK</c>).</summary>
        /// <param name="useLoopback"><c>true</c> to use the loopback where possible, <c>false</c> otherwise.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>setsockopt</c>.</returns>
        WinsockErrorCode SetOptionUseLoopback(bool useLoopback) noexcept
        {
            return SetOption(OptionLevel, SO_USELOOPBACK, useLoopback);
        }

        /// <summary>Gets the use-loopback setting (<c>SO_USELOOPBACK</c>).</summary>
        /// <param name="useLoopback">Required. Receives <c>true</c> if the loopback is used.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>getsockopt</c>.</returns>
        WinsockErrorCode GetOptionUseLoopback(bool* useLoopback) noexcept
        {
            return GetOption(OptionLevel, SO_USELOOPBACK, useLoopback);
        }

#pragma endregion

        /// <summary>Gets the result of an overlapped operation (<c>WSAGetOverlappedResult</c>).</summary>
        /// <param name="overlapped">The <c>OVERLAPPED</c> the operation was started with.</param>
        /// <param name="bytesTransferred">Required. Receives the bytes transferred.</param>
        /// <param name="wait">
        /// <c>TRUE</c> blocks the calling thread until the operation has completed; <c>FALSE</c> returns
        /// <c>WSA_IO_INCOMPLETE</c> while pending.
        /// </param>
        /// <param name="flags">Receives the operation's result flags; <c>nullptr</c> if they are not wanted.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/>, <see cref="WinsockError::Incomplete"/> (still pending,
        /// <paramref name="wait"/> <c>FALSE</c>) or the operation's error.
        /// </returns>
        WinsockErrorCode GetOverlappedResult(OVERLAPPED& overlapped, DWORD* bytesTransferred, BOOL wait, DWORD* flags) const noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(bytesTransferred);
            // WSAGetOverlappedResult faults on a null flags pointer
            DWORD unusedFlags;
            return WinsockError::GetAsyncError(WSAGetOverlappedResult(socket_, &overlapped, bytesTransferred, wait, flags ? flags : &unusedFlags));
        }

        /// <summary>Starts an overlapped <c>WSAIoctl</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="ioControlCode">The control code, e.g. a <c>SIO_*</c> constant.</param>
        /// <param name="inBuffer">The input data of the control code; <c>nullptr</c> if it takes none.</param>
        /// <param name="inBufferSize">The size of <paramref name="inBuffer"/> in bytes.</param>
        /// <param name="outBuffer">
        /// Receives the output data; <c>nullptr</c> if the control code returns none. Must stay valid until completion.
        /// </param>
        /// <param name="outBufferSize">The size of <paramref name="outBuffer"/> in bytes.</param>
        /// <param name="bytesReturned">Required. Receives the output byte count on synchronous completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// or the WSA error.
        /// </returns>
        /// <seealso cref="Ioctl"/>
        WinsockErrorCode IoctlAsync(DWORD ioControlCode, void* inBuffer, DWORD inBufferSize, void* outBuffer, DWORD outBufferSize, DWORD* bytesReturned,
                                    OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(bytesReturned);

            return WinsockError::GetError(
                WSAIoctl(socket_, ioControlCode, inBuffer, inBufferSize, outBuffer, outBufferSize, bytesReturned, &overlapped, nullptr));
        }

        /// <summary>Performs a <c>WSAIoctl</c> synchronously.</summary>
        /// <param name="ioControlCode">The control code, e.g. a <c>SIO_*</c> constant.</param>
        /// <param name="inBuffer">The input data of the control code; <c>nullptr</c> if it takes none.</param>
        /// <param name="inBufferSize">The size of <paramref name="inBuffer"/> in bytes.</param>
        /// <param name="outBuffer">Receives the output data; <c>nullptr</c> if the control code returns none.</param>
        /// <param name="outBufferSize">The size of <paramref name="outBuffer"/> in bytes.</param>
        /// <param name="bytesReturned">Required. Receives the number of bytes written to <paramref name="outBuffer"/>.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>WSAIoctl</c>.</returns>
        /// <seealso cref="IoctlAsync"/>
        WinsockErrorCode Ioctl(DWORD ioControlCode, void* inBuffer, DWORD inBufferSize, void* outBuffer, DWORD outBufferSize, DWORD* bytesReturned) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(bytesReturned);

            return WinsockError::GetError(WSAIoctl(socket_, ioControlCode, inBuffer, inBufferSize, outBuffer, outBufferSize, bytesReturned, nullptr, nullptr));
        }

        /// <summary>Loads a Winsock extension function pointer (<c>SIO_GET_EXTENSION_FUNCTION_POINTER</c>).</summary>
        /// <typeparam name="T">The function pointer type, e.g. <c>LPFN_ACCEPTEX</c>.</typeparam>
        /// <param name="functionGuid">The extension function's GUID, e.g. <c>WSAID_ACCEPTEX</c>.</param>
        /// <param name="functionPointer">Receives the function pointer.</param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>WSAIoctl</c>.</returns>
        template <class T>
        WinsockErrorCode IoctlGetExtensionFunctionPointer(GUID& functionGuid, T& functionPointer) noexcept
        {
            DWORD bytesReceived;
            return Ioctl(SIO_GET_EXTENSION_FUNCTION_POINTER, &functionGuid, sizeof(functionGuid), &functionPointer, sizeof(functionPointer), &bytesReceived);
        }

        /// <summary>Puts the bound socket into listening state (<c>listen</c>).</summary>
        /// <param name="backlog">
        /// The maximum length of the queue of pending connections; <c>SOMAXCONN</c> lets the provider choose.
        /// </param>
        /// <returns><see cref="WinsockError::Success"/>, or the WSA error of <c>listen</c>.</returns>
        /// <seealso cref="AcceptAsync"/>
        WinsockErrorCode Listen(int backlog = SOMAXCONN) noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetError(listen(socket_, backlog));
        }

        /// <summary>Requests cancellation of all overlapped operations on this socket (<c>CancelIoEx</c>).</summary>
        /// <returns>
        /// <see cref="WinsockError::Success"/> if cancellation was requested, <c>ERROR_NOT_FOUND</c> if no
        /// operation was pending, or another error.
        /// </returns>
        /// <remarks>
        /// Covers operations issued by any thread. Does not wait: each cancelled operation still completes
        /// (normally with <see cref="WinsockError::OperationAborted"/>); keep its <c>OVERLAPPED</c>/request and
        /// buffers alive until then.
        /// </remarks>
        /// <seealso cref="Close"/>
        WinsockErrorCode CancelAllAsyncRequests() noexcept
        {
            CT_ASSERT(IsValid());

            return WinsockError::GetAsyncError(CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr));
        }

        /// <summary>Starts a <c>WSARecv</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);
            CT_ASSERT(flags);

            return WinsockError::GetError(WSARecv(socket_, buffers, bufferCount, bytesTransferred, flags, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecv</c> without flags (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            DWORD flags = 0;
            return WinsockError::GetError(WSARecv(socket_, buffers, bufferCount, bytesTransferred, &flags, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecv</c> into one buffer (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvAsync(&wsabuf, 1, bytesTransferred, flags, overlapped);
        }

        /// <summary>Starts a <c>WSARecv</c> into one buffer without flags (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvAsync(&wsabuf, 1, bytesTransferred, overlapped);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> without the sender's address (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);
            CT_ASSERT(flags);

            return WinsockError::GetError(WSARecvFrom(socket_, buffers, bufferCount, bytesTransferred, flags, nullptr, nullptr, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecvFrom</c> without flags or sender address.</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            DWORD flags = 0;
            return WinsockError::GetError(WSARecvFrom(socket_, buffers, bufferCount, bytesTransferred, &flags, nullptr, nullptr, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without the sender's address.</summary>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvFromAsync(&wsabuf, 1, bytesTransferred, flags, overlapped);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without flags or sender address.</summary>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvFromAsync(&wsabuf, 1, bytesTransferred, overlapped);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> with the sender's address (raw <c>OVERLAPPED</c> form).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address on completion; must stay valid until then.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr, int& addrLength,
                                       OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);
            CT_ASSERT(flags);

            addrLength = sizeof(addr);
            return WinsockError::GetError(
                WSARecvFrom(socket_, buffers, bufferCount, bytesTransferred, flags, reinterpret_cast<SOCKADDR*>(&addr), &addrLength, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecvFrom</c> without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address on completion; must stay valid until then.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, _SockAddr& addr, int& addrLength,
                                       OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            DWORD flags = 0;
            addrLength  = sizeof(addr);
            return WinsockError::GetError(
                WSARecvFrom(socket_, buffers, bufferCount, bytesTransferred, &flags, reinterpret_cast<SOCKADDR*>(&addr), &addrLength, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags">
        /// Required. In: <c>MSG_*</c> flags for the call; the result flags come from <see cref="GetOverlappedResult"/>.
        /// </param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address on completion; must stay valid until then.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD* flags, _SockAddr& addr, int& addrLength,
                                       OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvFromAsync(&wsabuf, 1, bytesTransferred, flags, addr, addrLength, overlapped);
        }

        /// <summary>Starts a <c>WSARecvFrom</c> into one buffer without flags, with the sender's address.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the buffer length.</typeparam>
        /// <param name="buffer">Receives the data; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes received on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="addr">Receives the sender's address on completion; must stay valid until then.</param>
        /// <param name="addrLength">Receives the size of the sender's address on completion; must stay valid until then.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode RecvFromAsync(void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, _SockAddr& addr, int& addrLength,
                                       OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, static_cast<CHAR*>(buffer)};
            return RecvFromAsync(&wsabuf, 1, bytesTransferred, addr, addrLength, overlapped);
        }

        /// <summary>Starts a <c>WSASend</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            return WinsockError::GetError(WSASend(socket_, buffers, bufferCount, bytesTransferred, flags, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSASend</c> without flags.</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            return SendAsync(buffers, bufferCount, bytesTransferred, 0, overlapped);
        }

        /// <summary>Starts a <c>WSASend</c> of one buffer.</summary>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASend</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, const_cast<CHAR*>(static_cast<const CHAR*>(buffer))};
            return SendAsync(&wsabuf, 1, bytesTransferred, flags, overlapped);
        }

        /// <summary>Starts a <c>WSASend</c> of one buffer without flags.</summary>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            return SendAsync(buffer, bufferSize, bytesTransferred, 0, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> without address (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>Sends to the connected/default peer.</remarks>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            return WinsockError::GetError(WSASendTo(socket_, buffers, bufferCount, bytesTransferred, flags, nullptr, 0, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSASendTo</c> without address or flags.</summary>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            return SendToAsync(buffers, bufferCount, bytesTransferred, 0, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer without address.</summary>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, const_cast<CHAR*>(static_cast<const CHAR*>(buffer))};
            return SendToAsync(&wsabuf, 1, bytesTransferred, flags, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer without address or flags.</summary>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            return SendToAsync(buffer, bufferSize, bytesTransferred, 0, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> to an address (raw <c>OVERLAPPED</c> form).</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c>.</param>
        /// <param name="addr">The destination address.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                     OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(buffers);

            return WinsockError::GetError(
                WSASendTo(socket_, buffers, bufferCount, bytesTransferred, flags, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr), &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSASendTo</c> to an address without flags.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="buffers">
        /// Required. The <c>WSABUF</c> array may be temporary; the memory it describes must stay valid until completion.
        /// </param>
        /// <param name="bufferCount">The number of entries in <paramref name="buffers"/>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="addr">The destination address.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(WSABUF* buffers, DWORD bufferCount, DWORD* bytesTransferred, const _SockAddr& addr, OVERLAPPED& overlapped) noexcept
        {
            return SendToAsync(buffers, bufferCount, bytesTransferred, 0, addr, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer to an address.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendTo</c>.</param>
        /// <param name="addr">The destination address.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, DWORD flags, const _SockAddr& addr,
                                     OVERLAPPED& overlapped) noexcept
        {
            WSABUF wsabuf{bufferSize, const_cast<CHAR*>(static_cast<const CHAR*>(buffer))};
            return SendToAsync(&wsabuf, 1, bytesTransferred, flags, addr, overlapped);
        }

        /// <summary>Starts a <c>WSASendTo</c> of one buffer to an address without flags.</summary>
        /// <typeparam name="_SockAddr">The address structure type; its size is passed as the address length.</typeparam>
        /// <param name="buffer">The data to send; must stay valid until completion.</param>
        /// <param name="bufferSize">The size of <paramref name="buffer"/> in bytes.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="addr">The destination address.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        template <SockAddrType _SockAddr>
        WinsockErrorCode SendToAsync(const void* buffer, std::uint32_t bufferSize, DWORD* bytesTransferred, const _SockAddr& addr, OVERLAPPED& overlapped) noexcept
        {
            return SendToAsync(buffer, bufferSize, bytesTransferred, 0, addr, overlapped);
        }

        /// <summary>Starts a <c>WSARecvMsg</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="msg">Written at completion; must stay valid until then.</param>
        /// <param name="bytesTransferred">Required. Receives the bytes received on synchronous completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode RecvMsgAsync(WSAMSG* msg, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(msg);
            CT_ASSERT(bytesTransferred);

            return WinsockError::GetError(WSARecvMsg(socket_, msg, bytesTransferred, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>WSASendMsg</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="msg">It and its data must stay valid until completion.</param>
        /// <param name="flags"><c>MSG_*</c> flags for <c>WSASendMsg</c>.</param>
        /// <param name="bytesTransferred">Receives the bytes sent on synchronous completion; may be <c>nullptr</c>.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        WinsockErrorCode SendMsgAsync(WSAMSG* msg, DWORD flags, DWORD* bytesTransferred, OVERLAPPED& overlapped) noexcept
        {
            CT_ASSERT(IsValid());
            CT_ASSERT(msg);

            return WinsockError::GetError(WSASendMsg(socket_, msg, flags, bytesTransferred, &overlapped, nullptr));
        }

        /// <summary>Starts a <c>TransmitFile</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="file">
        /// The open file to send; must stay open until completion. <c>nullptr</c> sends only
        /// <paramref name="transmitBuffers"/>.
        /// </param>
        /// <param name="bytesToWrite">
        /// Bytes of the file to send; <c>0</c> sends the whole file from the offset in <paramref name="overlapped"/>.
        /// </param>
        /// <param name="bytesPerSend">Size of each send; <c>0</c> lets the system choose.</param>
        /// <param name="transmitBuffers">Optional head/tail data; must stay valid until completion.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; its <c>Offset</c>/<c>OffsetHigh</c> give the file position
        /// to start at. Must stay valid and must not be reused until the operation has completed.
        /// </param>
        /// <param name="flags"><c>TF_*</c> flags, e.g. <c>TF_DISCONNECT</c> | <c>TF_REUSE_SOCKET</c>.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>See also <see cref="AsyncIo::AsyncSocket::TransmitFileAsync"/>.</remarks>
        WinsockErrorCode TransmitFileAsync(HANDLE file, DWORD bytesToWrite, DWORD bytesPerSend, TRANSMIT_FILE_BUFFERS* transmitBuffers,
                                           OVERLAPPED& overlapped, DWORD flags = 0) noexcept
        {
            CT_ASSERT(IsValid());

            BOOL success = TransmitFile(socket_, file, bytesToWrite, bytesPerSend, &overlapped, transmitBuffers, flags);
            return WinsockError::GetAsyncError(success);
        }

        /// <summary>Starts a <c>TransmitPackets</c> (raw <c>OVERLAPPED</c> form).</summary>
        /// <param name="packetArray">
        /// The elements (memory and/or file ranges); they and everything they reference must stay valid until completion.
        /// </param>
        /// <param name="elementCount">The number of entries in <paramref name="packetArray"/>.</param>
        /// <param name="transmitSize">Bytes per send; <c>0</c> lets the system choose.</param>
        /// <param name="overlapped">
        /// The caller's <c>OVERLAPPED</c> for this operation; must stay valid and must not be reused until the
        /// operation has completed.
        /// </param>
        /// <param name="flags"><c>TF_*</c> flags.</param>
        /// <returns>
        /// <see cref="WinsockError::Success"/> (completed synchronously), <see cref="WinsockError::Pending"/>
        /// (in flight) or the WSA error.
        /// </returns>
        /// <remarks>See also <see cref="AsyncIo::AsyncSocket::TransmitPacketsAsync"/>.</remarks>
        WinsockErrorCode TransmitPacketsAsync(TRANSMIT_PACKETS_ELEMENT* packetArray, DWORD elementCount, DWORD transmitSize,
                                              OVERLAPPED& overlapped, DWORD flags = 0) noexcept
        {
            CT_ASSERT(IsValid());

            BOOL success = TransmitPackets(socket_, packetArray, elementCount, transmitSize, &overlapped, flags);
            return WinsockError::GetAsyncError(success);
        }

    protected:
        /// <summary>The raw <c>SOCKET</c>; stays owned by this object.</summary>
        /// <returns>The socket handle, or <c>INVALID_SOCKET</c> if the object holds none.</returns>
        SOCKET GetSocket() const noexcept { return socket_; }

    private:
        explicit Socket(SOCKET socket) noexcept : socket_(socket) {}

        static constexpr SOCKET InvalidSocket = INVALID_SOCKET;

        SOCKET socket_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows