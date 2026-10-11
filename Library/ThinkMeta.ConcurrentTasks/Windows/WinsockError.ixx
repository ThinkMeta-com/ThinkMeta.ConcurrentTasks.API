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

        /// <summary><c>true</c> for <see cref="Success"/>.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Success"/> (<c>0</c>); otherwise <c>false</c>.</returns>
        /// <seealso cref="Failed"/>
        static bool Succeeded(const Type& error) noexcept { return error == Success; }

        /// <summary><c>true</c> for anything but <see cref="Success"/>, including <see cref="Pending"/>.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is not <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <seealso cref="Succeeded"/>
        /// <seealso cref="IsPending"/>
        static bool Failed(const Type& error) noexcept { return !Succeeded(error); }

        /// <summary><c>true</c> for <see cref="Pending"/>: an overlapped operation is in flight.</summary>
        /// <param name="error">The error code to test.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Pending"/> (<c>WSA_IO_PENDING</c>); otherwise <c>false</c>.</returns>
        static bool IsPending(const Type& error) noexcept { return error == Pending; }
    };

    /// <summary>A Winsock error code as returned by the socket classes (<c>0</c> = <see cref="WinsockError::Success"/>).</summary>
    /// <seealso cref="WinsockError"/>
    export using WinsockErrorCode = WinsockError::Type;

    /// <summary>Exception carrying a <see cref="WinsockErrorCode"/>.</summary>
    /// <remarks>For callers that prefer exceptions over error codes.</remarks>
    export class WinsockErrorException
    {
    public:
        /// <summary>Wraps <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The Winsock error code to carry, normally a failure code.</param>
        explicit WinsockErrorException(WinsockErrorCode errorCode) noexcept : errorCode_{errorCode} {}

        /// <summary>The wrapped error code.</summary>
        /// <returns>The error code passed to the constructor.</returns>
        WinsockErrorCode GetErrorCode() const noexcept { return errorCode_; }

#ifdef _CPPUNWIND
        /// <summary>Throws a <see cref="WinsockErrorException"/> for <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The Winsock error code the exception carries.</param>
        [[noreturn]] static void Throw(WinsockErrorCode errorCode) { throw WinsockErrorException(errorCode); }
#else
        /// <summary>Without exception support: ends the process (<c>__fastfail</c>) instead of throwing.</summary>
        /// <param name="errorCode">The Winsock error code; not used, the process ends.</param>
        /// <remarks>There is nothing to unwind, so a failure the caller cannot be told about ends the process here.</remarks>
        [[noreturn]] static void Throw([[maybe_unused]] WinsockErrorCode errorCode) noexcept { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }
#endif

        /// <summary>Throws if <paramref name="errorCode"/> is not <see cref="WinsockError::Success"/>.</summary>
        /// <param name="errorCode">The Winsock error code to check; any value but <see cref="WinsockError::Success"/> throws.</param>
        /// <remarks>Without exception support it ends the process instead.</remarks>
        /// <seealso cref="Throw"/>
        static void ThrowIfFailed(WinsockErrorCode errorCode)
        {
            if (WinsockError::Failed(errorCode))
                Throw(errorCode);
        }

    private:
        const WinsockErrorCode errorCode_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows