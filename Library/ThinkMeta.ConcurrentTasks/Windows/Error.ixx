//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Win32 error codes as the library reports them: the Error helper (named codes and predicates),
// the ErrorCode alias, and ErrorException for code paths that cannot return an error.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

export module ThinkMeta.ConcurrentTasks.Windows.Error;

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
        static constexpr Type OperationAborted  = ERROR_OPERATION_ABORTED;

        /// <summary><c>ERROR_TIMEOUT</c>: an operation with a timeout ran out of time.</summary>
        /// <remarks>
        /// Reported by the library's own timed operations (<see cref="AsyncIo::AsyncFile::Read"/>, say).
        /// The Win32 wait functions behind <see cref="Thread::Wait"/> and
        /// <see cref="IoCompletionPort::GetQueuedCompletionStatus"/> report their timeout as
        /// <see cref="WaitTimedOut"/> (<c>WAIT_TIMEOUT</c>, 258) instead.
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
        static bool Succeeded(const Type& error) noexcept { return error == Success; }

        /// <summary>Returns whether <paramref name="error"/> is anything but <see cref="Success"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> for every code other than <see cref="Success"/>; otherwise <c>false</c>.</returns>
        /// <remarks><see cref="Pending"/> counts as failed.</remarks>
        /// <seealso cref="Succeeded"/>
        static bool Failed(const Type& error) noexcept { return !Succeeded(error); }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="Pending"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <c>ERROR_IO_PENDING</c>; otherwise <c>false</c>.</returns>
        static bool IsPending(const Type& error) noexcept { return error == Pending; }

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
        static bool TooManyAsynchronousRequests(const Type& error) noexcept { return error == InvalidUserBuffer || error == NotEnoughMemory; }

        /// <summary>Returns whether <paramref name="error"/> is <see cref="EndOfStream"/>.</summary>
        /// <param name="error">A Win32 error code.</param>
        /// <returns><c>true</c> if <paramref name="error"/> is <c>ERROR_HANDLE_EOF</c>; otherwise <c>false</c>.</returns>
        static bool IsEndOfStream(const Type& error) noexcept { return error == EndOfStream; }
    };

    /// <summary>A Win32 error code as returned by the Windows wrappers.</summary>
    /// <remarks><see cref="Error::Success"/> on success.</remarks>
    /// <seealso cref="Error"/>
    export using ErrorCode = Error::Type;

    /// <summary>Carries a Win32 error out of code that has no error return.</summary>
    /// <remarks>
    /// With C++ exceptions enabled <see cref="Throw"/> throws it; without them (<c>/EHs-</c> etc., as
    /// in the CRT-free core) Throw ends the process.
    /// In code that cannot pass exceptions on (noexcept functions, such as most of the library)
    /// <see cref="ThrowIfFailed"/> ends the process: there it is a fail-fast check.
    /// </remarks>
    export class ErrorException
    {
    public:
        /// <summary>Constructs the exception for <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The Win32 error code to carry; normally not <see cref="Error::Success"/>.</param>
        explicit ErrorException(ErrorCode errorCode) noexcept : errorCode_{errorCode} {}

        /// <summary>Returns the Win32 error code the exception carries.</summary>
        /// <returns>The code passed to the constructor.</returns>
        ErrorCode GetErrorCode() const noexcept { return errorCode_; }

        /// <summary>Throws an <see cref="ErrorException"/> for <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The Win32 error code the exception carries.</param>
        /// <remarks>Built without exceptions, it ends the process instead.</remarks>
        /// <seealso cref="ThrowIfFailed"/>
#ifdef _CPPUNWIND
        [[noreturn]] static void Throw(ErrorCode errorCode) { throw ErrorException(errorCode); }
#else
        // Built without exceptions, as the CRT-free core is: there is nothing to unwind, so a
        // failure the caller cannot be told about ends the process right here.
        [[noreturn]] static void Throw([[maybe_unused]] ErrorCode errorCode) noexcept { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }
#endif

        /// <summary>Calls <see cref="Throw"/> unless the code is <see cref="Error::Success"/>.</summary>
        /// <param name="errorCode">The Win32 error code to check.</param>
        static void ThrowIfFailed(ErrorCode errorCode)
        {
            if (Error::Failed(errorCode))
                Throw(errorCode);
        }

    private:
        const ErrorCode errorCode_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows