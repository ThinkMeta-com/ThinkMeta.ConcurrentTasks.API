//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// _ErrorException: carries an error code out of code that has no error return. Shared by the error
// helpers of every platform (Windows::Error, Windows::WinsockError, Posix::Error), which name it
// for their own codes.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h> // CT_FASTFAIL: __fastfail, FAST_FAIL_FATAL_APP_EXIT
#endif

// C++ exceptions enabled: _CPPUNWIND with Visual Studio, __cpp_exceptions with clang
#if defined(_CPPUNWIND) || defined(__cpp_exceptions)
#    define CT_ERROR_EXCEPTIONS 1
#endif

export module ThinkMeta.ConcurrentTasks.Platform.ErrorException;

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>Carries an error code out of code that has no error return.</summary>
    /// <typeparam name="_Error">
    /// The error helper that names the codes, such as <see cref="Error"/>; it provides
    /// <c>Type</c> and <c>Failed</c>.
    /// </typeparam>
    /// <remarks>
    /// With C++ exceptions enabled <see cref="Throw"/> throws it; without them (as in the
    /// runtime-free core) Throw ends the process.
    /// In code that cannot pass exceptions on (noexcept functions, such as most of the library)
    /// <see cref="ThrowIfFailed"/> ends the process: there it is a fail-fast check.
    /// </remarks>
    /// <seealso cref="ErrorException"/>
    export template <typename _Error>
    class _ErrorException
    {
    public:
        /// <summary>The error code type the exception carries.</summary>
        using Code = typename _Error::Type;

        /// <summary>Constructs the exception for <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The error code to carry; normally a failure code.</param>
        explicit _ErrorException(Code errorCode) noexcept : errorCode_{errorCode} {}

        /// <summary>Returns the error code the exception carries.</summary>
        /// <returns>The code passed to the constructor.</returns>
        Code GetErrorCode() const noexcept { return errorCode_; }

        /// <summary>Throws the exception for <paramref name="errorCode"/>.</summary>
        /// <param name="errorCode">The error code the exception carries.</param>
        /// <remarks>Built without exceptions, it ends the process instead.</remarks>
        /// <seealso cref="ThrowIfFailed"/>
#ifdef CT_ERROR_EXCEPTIONS
        [[noreturn]] static void Throw(Code errorCode) { throw _ErrorException(errorCode); }
#else
        // Built without exceptions, as the runtime-free core is: there is nothing to unwind, so a
        // failure the caller cannot be told about ends the process right here.
        [[noreturn]] static void Throw([[maybe_unused]] Code errorCode) noexcept { CT_FASTFAIL(); }
#endif

        /// <summary>Calls <see cref="Throw"/> unless the code means success.</summary>
        /// <param name="errorCode">The error code to check.</param>
        /// <remarks>Built without exceptions, a failure ends the process instead.</remarks>
#ifdef CT_ERROR_EXCEPTIONS
        static void ThrowIfFailed(Code errorCode)
#else
        static void ThrowIfFailed(Code errorCode) noexcept
#endif
        {
            if (_Error::Failed(errorCode))
                Throw(errorCode);
        }

    private:
        const Code errorCode_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Platform
