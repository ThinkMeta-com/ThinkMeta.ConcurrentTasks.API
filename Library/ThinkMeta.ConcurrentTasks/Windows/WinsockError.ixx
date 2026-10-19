//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Winsock error codes: WinsockError names the WSA* values the socket classes return and maps
// Winsock return conventions to a WinsockErrorCode; WinsockErrorException is the optional
// throwing form.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <WinSock2.h>
#include <Windows.h>

export module ThinkMeta.ConcurrentTasks.Windows.WinsockError;

export import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Named Winsock error codes and helpers that turn Winsock return values into error codes.</summary>
    /// <remarks>
    /// A <see cref="WinsockErrorCode"/> is <see cref="Success"/> (<c>0</c>) or the <c>WSAGetLastError</c> value.
    /// <c>WSAGetLastError</c> is thread-local and equal to <c>GetLastError</c>, so the helpers must run
    /// right after the failing call on the same thread. For completed overlapped operations the socket
    /// classes use the completion's own code instead.
    /// </remarks>
    /// <seealso cref="WinsockErrorException"/>
    export struct WinsockError {
        /// <summary>The underlying error type: <c>int</c>, as <c>WSAGetLastError</c>.</summary>
        using Type = int;

        // Socket errors (WSAE*).
        static constexpr Type Success                   = 0;
        static constexpr Type NotInitialized            = WSANOTINITIALISED;
        static constexpr Type BadDescriptor             = WSAEBADF;
        static constexpr Type InvalidParameter          = WSAEINVAL;
        static constexpr Type TooManyOpenFiles          = WSAEMFILE;
        static constexpr Type WouldBlock                = WSAEWOULDBLOCK;
        static constexpr Type InProgress                = WSAEINPROGRESS;
        static constexpr Type AlreadyInProgress         = WSAEALREADY;
        static constexpr Type NotSocket                 = WSAENOTSOCK;
        static constexpr Type MessageSize               = WSAEMSGSIZE;
        static constexpr Type ProtocolType              = WSAEPROTOTYPE;
        static constexpr Type ProtocolOption            = WSAENOPROTOOPT;
        static constexpr Type OperationNotSupported     = WSAEOPNOTSUPP;
        static constexpr Type AddressFamilyNotSupported = WSAEAFNOSUPPORT;
        static constexpr Type AddressInUse              = WSAEADDRINUSE;
        static constexpr Type AddressNotAvailable       = WSAEADDRNOTAVAIL;

        /// <summary><c>WSA_IO_PENDING</c>: an overlapped operation was started and completes later.</summary>
        static constexpr Type Pending = WSA_IO_PENDING;

        /// <summary><c>WSA_IO_INCOMPLETE</c>: the operation is still pending.</summary>
        /// <remarks>Returned by <c>GetOverlappedResult</c> without wait.</remarks>
        static constexpr Type Incomplete        = WSA_IO_INCOMPLETE;
        static constexpr Type InvalidHandle     = WSA_INVALID_HANDLE;

        /// <summary><c>WSA_INVALID_PARAMETER</c> (<c>ERROR_INVALID_PARAMETER</c>).</summary>
        /// <remarks>Not the same value as <see cref="InvalidParameter"/> (<c>WSAEINVAL</c>).</remarks>
        static constexpr Type InvalidParameterW = WSA_INVALID_PARAMETER;
        static constexpr Type NotEnoughMemory   = WSA_NOT_ENOUGH_MEMORY;

        /// <summary><c>WSA_OPERATION_ABORTED</c>: the operation was cancelled or its socket closed.</summary>
        static constexpr Type OperationAborted = WSA_OPERATION_ABORTED;

        /// <summary><c>WSAETIMEDOUT</c>: the operation timed out.</summary>
        /// <remarks>Also returned by the <see cref="AsyncIo::AsyncSocket"/> waiting forms when their timeout expired.</remarks>
        static constexpr Type TimedOut = WSAETIMEDOUT;

        /// <summary>The error of a call returning <c>0</c> on success.</summary>
        /// <param name="result">The result of e.g. <c>bind</c> or <c>WSARecv</c>.</param>
        /// <returns><see cref="Success"/>, or <c>WSAGetLastError</c> for any other result.</returns>
        static Type GetError(int result) noexcept { return result == 0 ? Success : GetLastError(); }

        /// <summary>The error of a call returning <c>BOOL</c>.</summary>
        /// <param name="result">The result of e.g. <c>AcceptEx</c> or <c>ConnectEx</c>.</param>
        /// <returns><see cref="Success"/> if <c>TRUE</c>, else <c>WSAGetLastError</c>.</returns>
        static Type GetAsyncError(BOOL result) noexcept { return result ? Success : GetLastError(); }

        /// <summary>The calling thread's last Winsock error (<c>WSAGetLastError</c>).</summary>
        /// <returns>The last error code; only meaningful right after a failed Winsock call on this thread.</returns>
        static Type GetLastError() noexcept { return ::WSAGetLastError(); }

        /// <summary>The Winsock error of a completed overlapped operation.</summary>
        /// <param name="overlapped">The operation's <c>OVERLAPPED</c>; its <c>Internal</c> holds the final <c>NTSTATUS</c>.</param>
        /// <param name="error">The error reported for the completion; <see cref="Success"/> (<c>0</c>) if it succeeded.</param>
        /// <returns>
        /// <see cref="Success"/> if <paramref name="error"/> is; otherwise the <c>WSAE*</c> code Winsock gives the
        /// operation's status (what <c>WSAGetOverlappedResult</c> would report), or <paramref name="error"/>
        /// itself if the status is unknown here or no failure status is at hand.
        /// </returns>
        /// <remarks>
        /// A completion port reports a failed operation as a Win32 code made from its <c>NTSTATUS</c>
        /// (<c>ERROR_NETNAME_DELETED</c> for a reset, say), while the same failure of a synchronous call
        /// is a <c>WSAE*</c> code (<c>WSAECONNRESET</c>). This maps the status back to the Winsock code
        /// without a socket at hand. A failure the issuer reported itself is a <c>WSAE*</c> code
        /// already and leaves no failure status in the <c>OVERLAPPED</c>, so it passes unchanged.
        /// </remarks>
        static Type FromOverlapped(const OVERLAPPED& overlapped, Type error) noexcept
        {
            if (Succeeded(error))
                return Success;

            // NTSTATUS values spelled out: ntstatus.h clashes with Windows.h. Only failures are
            // mapped; a success or warning status leaves the reported code as it is.
            const auto status = static_cast<unsigned long>(overlapped.Internal);
            if (status < 0xC0000000)
                return error;

            switch (status) {
            case 0xC0000120: return WSA_OPERATION_ABORTED; // STATUS_CANCELLED
            case 0xC000020D:                               // STATUS_CONNECTION_RESET
            case 0xC000013C:                               // STATUS_REMOTE_DISCONNECT
            case 0xC000020C:                               // STATUS_CONNECTION_DISCONNECTED
            case 0xC000023F: return WSAECONNRESET;         // STATUS_PORT_UNREACHABLE
            case 0xC0000241:                               // STATUS_CONNECTION_ABORTED
            case 0xC000013B: return WSAECONNABORTED;       // STATUS_LOCAL_DISCONNECT
            case 0xC0000236: return WSAECONNREFUSED;       // STATUS_CONNECTION_REFUSED
            case 0xC0000237: return WSAEDISCON;            // STATUS_GRACEFUL_DISCONNECT
            case 0xC00000B5: return WSAETIMEDOUT;          // STATUS_IO_TIMEOUT
            case 0xC000023D: return WSAEHOSTUNREACH;       // STATUS_HOST_UNREACHABLE
            case 0xC0000350: return WSAEHOSTDOWN;          // STATUS_HOST_DOWN
            case 0xC000023C:                               // STATUS_NETWORK_UNREACHABLE
            case 0xC000023E:                               // STATUS_PROTOCOL_UNREACHABLE
            case 0xC00000BE: return WSAENETUNREACH;        // STATUS_BAD_NETWORK_PATH
            case 0xC000009A:                               // STATUS_INSUFFICIENT_RESOURCES
            case 0xC0000209: return WSAENOBUFS;            // STATUS_TOO_MANY_ADDRESSES
            case 0xC000020A: return WSAEADDRINUSE;         // STATUS_ADDRESS_ALREADY_EXISTS
            case 0xC0000207: return WSAEADDRNOTAVAIL;      // STATUS_INVALID_ADDRESS_COMPONENT
            case 0xC0000140:                               // STATUS_INVALID_CONNECTION
            case 0xC000023A: return WSAENOTCONN;           // STATUS_CONNECTION_INVALID
            case 0xC000023B: return WSAEISCONN;            // STATUS_CONNECTION_ACTIVE
            case 0xC0000022: return WSAEACCES;             // STATUS_ACCESS_DENIED
            case 0xC0000008: return WSAENOTSOCK;           // STATUS_INVALID_HANDLE
            case 0xC00000BB: return WSAEOPNOTSUPP;         // STATUS_NOT_SUPPORTED
            case 0xC000000D: return WSAEINVAL;             // STATUS_INVALID_PARAMETER
            default:         return error;
            }
        }

        /// <summary><c>true</c> for <see cref="Success"/>.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Success"/> (<c>0</c>); otherwise <c>false</c>.</returns>
        /// <seealso cref="Failed"/>
        static constexpr bool Succeeded(Type error) noexcept { return error == Success; }

        /// <summary><c>true</c> for anything but <see cref="Success"/>, including <see cref="Pending"/>.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is not <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <seealso cref="Succeeded"/>
        /// <seealso cref="IsPending"/>
        static constexpr bool Failed(Type error) noexcept { return !Succeeded(error); }

        /// <summary><c>true</c> for <see cref="Pending"/>: an overlapped operation is in flight.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Pending"/> (<c>WSA_IO_PENDING</c>); otherwise <c>false</c>.</returns>
        static constexpr bool IsPending(Type error) noexcept { return error == Pending; }
    };

    /// <summary>A Winsock error code as returned by the socket classes (<c>0</c> = <see cref="WinsockError::Success"/>).</summary>
    /// <seealso cref="WinsockError"/>
    export using WinsockErrorCode = WinsockError::Type;

    /// <summary>Carries a <see cref="WinsockErrorCode"/> out of code that has no error return.</summary>
    /// <remarks>For callers that prefer exceptions over error codes; see <see cref="_ErrorException"/>.</remarks>
    export using WinsockErrorException = _ErrorException<WinsockError>;
} // namespace ThinkMeta::ConcurrentTasks::Windows