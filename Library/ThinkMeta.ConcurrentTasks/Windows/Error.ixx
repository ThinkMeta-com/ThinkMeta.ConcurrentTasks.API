//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Win32 error codes as the library reports them: the Error helper (named codes and predicates),
// the ErrorCode alias, and ErrorException for code paths that cannot return an error (the class
// itself is shared by all platforms, Platform\PlatformErrorException.ixx).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

export module ThinkMeta.ConcurrentTasks.Windows.Error;

export import ThinkMeta.ConcurrentTasks.Platform.ErrorException;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Names and predicates for Win32 error codes.</summary>
    /// <remarks>
    /// The codes are the values of <c>GetLastError</c>. The Windows wrappers return an
    /// <see cref="ErrorCode"/>: <see cref="Success"/>, or the Win32 error of the failed call.
    /// </remarks>
    /// <seealso cref="ErrorException"/>
    export struct Error {
        /// <summary>A Win32 error code (<c>DWORD</c>).</summary>
        using Type = DWORD;

        /// <summary><c>ERROR_SUCCESS</c> (<c>0</c>): the operation succeeded.</summary>
        static constexpr Type Success           = ERROR_SUCCESS;
        static constexpr Type BadFormat         = ERROR_BAD_FORMAT;
        static constexpr Type InvalidBlock      = ERROR_INVALID_BLOCK;

        /// <summary><c>ERROR_HANDLE_EOF</c>: a read started at or beyond the end of the file.</summary>
        static constexpr Type EndOfFile = ERROR_HANDLE_EOF;

        /// <summary>Same value as <see cref="EndOfFile"/>, for streams and sockets.</summary>
        /// <seealso cref="IsEndOfStream"/>
        static constexpr Type EndOfStream = EndOfFile;

        /// <summary><c>ERROR_IO_PENDING</c>: an overlapped operation completes later.</summary>
        /// <seealso cref="IsPending"/>
        static constexpr Type Pending           = ERROR_IO_PENDING;
        static constexpr Type InvalidHandle     = ERROR_INVALID_HANDLE;
        static constexpr Type InvalidUserBuffer = ERROR_INVALID_USER_BUFFER;
        static constexpr Type NotEnoughMemory   = ERROR_NOT_ENOUGH_MEMORY;
        static constexpr Type InvalidParameter  = ERROR_INVALID_PARAMETER;
        static constexpr Type NotFound          = ERROR_NOT_FOUND;
        static constexpr Type FileNotFound      = ERROR_FILE_NOT_FOUND;
        static constexpr Type WriteFault        = ERROR_WRITE_FAULT;
        static constexpr Type OperationAborted  = ERROR_OPERATION_ABORTED;

        /// <summary><c>ERROR_TIMEOUT</c>: an operation with a timeout ran out of time.</summary>
        /// <remarks>
        /// Reported by the library's own timed operations (<see cref="AsyncIo::AsyncFile::Read"/>, say).
        /// <see cref="IoCompletionPort::GetQueuedCompletionStatus"/> reports its timeout as
        /// <see cref="WaitTimedOut"/> (<c>WAIT_TIMEOUT</c>, 258) instead; <see cref="Thread::Wait"/>
        /// returns <see cref="Success"/> and passes <c>WAIT_TIMEOUT</c> in its wait result.
        /// </remarks>
        static constexpr Type TimedOut = ERROR_TIMEOUT;

        /// <summary><c>WAIT_TIMEOUT</c>: a Win32 wait ran out of time (see <see cref="TimedOut"/>).</summary>
        static constexpr Type WaitTimedOut = WAIT_TIMEOUT;

        /// <summary>Returns the calling thread's last Win32 error (<c>::GetLastError</c>).</summary>
        /// <returns>The thread's last Win32 error code; <see cref="Success"/> if none was set.</returns>
        /// <seealso cref="GetError"/>
        static Type GetLastError() noexcept { return ::GetLastError(); }

        /// <summary>Turns a Win32 <c>BOOL</c> result into an <see cref="ErrorCode"/>.</summary>
        /// <param name="b">The Win32 call's result: nonzero for success, <c>FALSE</c> for failure.</param>
        /// <returns><see cref="Success"/> for <c>TRUE</c>, else <see cref="GetLastError"/>.</returns>
        /// <remarks>
        /// Call it right after the API call, before anything else can overwrite the last error.
        /// </remarks>
        static Type GetError(BOOL b) noexcept { return b ? Success : GetLastError(); }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="Success"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <seealso cref="Failed"/>
        static constexpr bool Succeeded(Type error) noexcept { return error == Success; }

        /// <summary>Returns whether <paramref name="error"/> is anything but <see cref="Success"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> for every code other than <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <remarks><see cref="Pending"/> counts as failed.</remarks>
        /// <seealso cref="Succeeded"/>
        static constexpr bool Failed(Type error) noexcept { return !Succeeded(error); }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="Pending"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <c>ERROR_IO_PENDING</c>; otherwise <c>false</c>.</returns>
        static constexpr bool IsPending(Type error) noexcept { return error == Pending; }

        /// <summary>Returns whether <paramref name="error"/> means too many overlapped requests.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns>
        /// <c>true</c> for <c>ERROR_INVALID_USER_BUFFER</c> and <c>ERROR_NOT_ENOUGH_MEMORY</c>; otherwise
        /// <c>false</c>.
        /// </returns>
        /// <remarks>
        /// These are the codes Windows uses when too many overlapped requests are outstanding
        /// (<c>ERROR_INVALID_USER_BUFFER</c>, <c>ERROR_NOT_ENOUGH_MEMORY</c>): retry later.
        /// </remarks>
        static constexpr bool TooManyAsynchronousRequests(Type error) noexcept { return error == InvalidUserBuffer || error == NotEnoughMemory; }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="EndOfStream"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <c>ERROR_HANDLE_EOF</c>; otherwise <c>false</c>.</returns>
        static constexpr bool IsEndOfStream(Type error) noexcept { return error == EndOfStream; }
    };

    /// <summary>A Win32 error code as returned by the Windows wrappers.</summary>
    /// <remarks><see cref="Error::Success"/> on success.</remarks>
    /// <seealso cref="Error"/>
    export using ErrorCode = Error::Type;

    /// <summary>Carries an error code out of code that has no error return.</summary>
    /// <typeparam name="_Error">
    /// The error helper that names the codes: <see cref="Error"/> (Win32) or <see cref="WinsockError"/> (Winsock).
    /// </typeparam>
    /// <remarks>The class is shared by all platforms, see <see cref="Platform::_ErrorException"/>.</remarks>
    /// <seealso cref="ErrorException"/>
    /// <seealso cref="WinsockErrorException"/>
    export template <typename _Error>
    using _ErrorException = Platform::_ErrorException<_Error>;

    /// <summary>Carries a Win32 error out of code that has no error return.</summary>
    /// <remarks>See <see cref="_ErrorException"/>.</remarks>
    export using ErrorException = _ErrorException<Error>;
} // namespace ThinkMeta::ConcurrentTasks::Windows