//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// POSIX error codes as the library reports them: the Error helper (named codes and predicates),
// the ErrorCode alias, and ErrorException for code paths that cannot return an error. The
// counterpart of Windows\Error.ixx; code that runs on both uses the names of Platform.Error.
//
// The names are those of Windows::Error, so code that runs on both compares through them. Two
// codes have no errno of their own and are made up (negative, where no errno lies): the end of a
// file, which Linux reports as a read of 0 bytes, and an operation that is still pending, which
// the io_uring of the core reports by its completion only.
//
// The file is not called Error.ixx: file names stay unique across the library, since Visual Studio
// puts the objects of a project into one folder (MSB8027).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <cerrno>
#include <cstdint>

export module ThinkMeta.ConcurrentTasks.Posix.Error;

export import ThinkMeta.ConcurrentTasks.Platform.ErrorException;

namespace ThinkMeta::ConcurrentTasks::Posix {
    /// <summary>Names and predicates for POSIX error codes.</summary>
    /// <remarks>
    /// The codes are the values of <c>errno</c>. The POSIX wrappers return an
    /// <see cref="ErrorCode"/>: <see cref="Success"/>, or the <c>errno</c> of the failed call.
    /// </remarks>
    /// <seealso cref="ErrorException"/>
    export struct Error {
        /// <summary>A POSIX error code (an <c>errno</c> value).</summary>
        using Type = int;

        /// <summary><c>0</c>: the operation succeeded.</summary>
        static constexpr Type Success = 0;

        /// <summary>A read started at or beyond the end of the file (made up: Linux reads 0 bytes there).</summary>
        static constexpr Type EndOfFile = -1;

        /// <summary>Same value as <see cref="EndOfFile"/>, for streams and sockets.</summary>
        /// <seealso cref="IsEndOfStream"/>
        static constexpr Type EndOfStream = EndOfFile;

        /// <summary>An asynchronous operation was started and completes later (made up).</summary>
        /// <seealso cref="IsPending"/>
        static constexpr Type Pending = -2;

        /// <summary><c>EBADF</c>: not an open file or socket.</summary>
        static constexpr Type InvalidHandle = EBADF;

        /// <summary><c>EFAULT</c>: a buffer address is outside the process's memory.</summary>
        static constexpr Type InvalidUserBuffer = EFAULT;

        /// <summary><c>ENOMEM</c>: not enough memory.</summary>
        static constexpr Type NotEnoughMemory = ENOMEM;

        /// <summary><c>EINVAL</c>: a parameter is not valid.</summary>
        static constexpr Type InvalidParameter = EINVAL;

        /// <summary><c>ENOENT</c>: nothing of that name.</summary>
        static constexpr Type NotFound = ENOENT;

        /// <summary><c>ENOENT</c>: the file does not exist.</summary>
        static constexpr Type FileNotFound = ENOENT;

        /// <summary><c>EIO</c>: the device could not write (also: a write that transferred less than asked).</summary>
        static constexpr Type WriteFault = EIO;

        // The socket errors under the names Windows::WinsockError gives them.

        /// <summary><c>EBADF</c>, as <c>WSAEBADF</c>.</summary>
        static constexpr Type BadDescriptor = EBADF;

        /// <summary><c>EMFILE</c>: the process has too many open files.</summary>
        static constexpr Type TooManyOpenFiles = EMFILE;

        /// <summary><c>EWOULDBLOCK</c>: a non-blocking call would have to wait.</summary>
        static constexpr Type WouldBlock = EWOULDBLOCK;

        /// <summary><c>EINPROGRESS</c>: a non-blocking connect is under way.</summary>
        static constexpr Type InProgress = EINPROGRESS;

        /// <summary><c>EALREADY</c>: an operation of this kind is already under way.</summary>
        static constexpr Type AlreadyInProgress = EALREADY;

        /// <summary><c>ENOTSOCK</c>: the descriptor is no socket.</summary>
        static constexpr Type NotSocket = ENOTSOCK;

        /// <summary><c>EMSGSIZE</c>: a message too long for the socket.</summary>
        static constexpr Type MessageSize = EMSGSIZE;

        /// <summary><c>EPROTOTYPE</c>: a protocol that does not fit the socket type.</summary>
        static constexpr Type ProtocolType = EPROTOTYPE;

        /// <summary><c>ENOPROTOOPT</c>: an unknown socket option.</summary>
        static constexpr Type ProtocolOption = ENOPROTOOPT;

        /// <summary><c>EOPNOTSUPP</c>: the operation is not supported (also: the kernel cannot cancel by file).</summary>
        static constexpr Type OperationNotSupported = EOPNOTSUPP;

        /// <summary><c>EAFNOSUPPORT</c>: an address family not supported.</summary>
        static constexpr Type AddressFamilyNotSupported = EAFNOSUPPORT;

        /// <summary><c>EADDRINUSE</c>: the address is in use.</summary>
        static constexpr Type AddressInUse = EADDRINUSE;

        /// <summary><c>EADDRNOTAVAIL</c>: the address is not available on this host.</summary>
        static constexpr Type AddressNotAvailable = EADDRNOTAVAIL;

        /// <summary><c>ECONNABORTED</c>: the connection was aborted on this side.</summary>
        static constexpr Type ConnectionAborted = ECONNABORTED;

        /// <summary><c>ECONNRESET</c>: the peer reset the connection.</summary>
        static constexpr Type ConnectionReset = ECONNRESET;

        /// <summary><c>ENOTCONN</c>: the socket is not connected.</summary>
        static constexpr Type NotConnected = ENOTCONN;

        /// <summary><c>ECONNREFUSED</c>: nothing listens at the address.</summary>
        static constexpr Type ConnectionRefused = ECONNREFUSED;

        /// <summary><c>ECANCELED</c>: the operation was cancelled before it completed.</summary>
        static constexpr Type OperationAborted = ECANCELED;

        /// <summary><c>ETIMEDOUT</c>: an operation with a timeout ran out of time.</summary>
        static constexpr Type TimedOut = ETIMEDOUT;

        /// <summary>Same value as <see cref="TimedOut"/>; Windows tells a wait's timeout apart.</summary>
        static constexpr Type WaitTimedOut = ETIMEDOUT;

        /// <summary>Returns the calling thread's last error (<c>errno</c>).</summary>
        /// <returns>The thread's <c>errno</c>.</returns>
        /// <seealso cref="GetError"/>
        static Type GetLastError() noexcept { return errno; }

        /// <summary>Turns the result of a call into an <see cref="ErrorCode"/>.</summary>
        /// <param name="succeeded">Whether the call succeeded.</param>
        /// <returns><see cref="Success"/> if it did, else <see cref="GetLastError"/>.</returns>
        /// <remarks>Call it right after the call, before anything else can overwrite <c>errno</c>.</remarks>
        static Type GetError(bool succeeded) noexcept { return succeeded ? Success : GetLastError(); }

        /// <summary>Not available: a result such as <c>0</c>/<c>-1</c> must be compared first.</summary>
        /// <remarks>
        /// <c>Windows::WinsockError::GetError(int)</c> takes the result itself (<c>0</c>: success); here an
        /// <c>int</c> would turn into <c>bool</c> the other way round. Pass <c>call(...) == 0</c> instead.
        /// </remarks>
        static Type GetError(int result) = delete;

        /// <summary>Returns whether <paramref name="error"/> is <see cref="Success"/>.</summary>
        /// <param name="error">A POSIX error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <seealso cref="Failed"/>
        static constexpr bool Succeeded(Type error) noexcept { return error == Success; }

        /// <summary>Returns whether <paramref name="error"/> is anything but <see cref="Success"/>.</summary>
        /// <param name="error">A POSIX error code.</param>
        /// <returns><c>true</c> for every code other than <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <seealso cref="Succeeded"/>
        static constexpr bool Failed(Type error) noexcept { return !Succeeded(error); }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="Pending"/>.</summary>
        /// <param name="error">A POSIX error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Pending"/>; otherwise <c>false</c>.</returns>
        static constexpr bool IsPending(Type error) noexcept { return error == Pending; }

        /// <summary>Returns whether <paramref name="error"/> means the kernel takes no more operations for now.</summary>
        /// <param name="error">A POSIX error code.</param>
        /// <returns><c>true</c> for <c>EAGAIN</c> and <c>EBUSY</c>; otherwise <c>false</c>.</returns>
        /// <remarks>Retry later, as on Windows.</remarks>
        static constexpr bool TooManyAsynchronousRequests(Type error) noexcept { return error == EAGAIN || error == EBUSY; }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="EndOfStream"/>.</summary>
        /// <param name="error">A POSIX error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="EndOfStream"/>; otherwise <c>false</c>.</returns>
        static constexpr bool IsEndOfStream(Type error) noexcept { return error == EndOfStream; }

        /// <summary>Turns the result of an asynchronous operation of the core into an error code.</summary>
        /// <param name="result">What the completion routine received: a byte count, or a negated <c>errno</c>.</param>
        /// <returns><see cref="Success"/> for a byte count; otherwise the <c>errno</c>.</returns>
        static constexpr Type FromResult(std::int32_t result) noexcept { return result < 0 ? -result : Success; }
    };

    /// <summary>A POSIX error code as returned by the POSIX wrappers.</summary>
    /// <remarks><see cref="Error::Success"/> on success.</remarks>
    /// <seealso cref="Error"/>
    export using ErrorCode = Error::Type;

    /// <summary>Carries a POSIX error out of code that has no error return.</summary>
    /// <remarks>See <see cref="Platform::_ErrorException"/>.</remarks>
    export using ErrorException = Platform::_ErrorException<Error>;
} // namespace ThinkMeta::ConcurrentTasks::Posix
