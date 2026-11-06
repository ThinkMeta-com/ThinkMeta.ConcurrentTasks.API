//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Error: the error codes of the platform the library is built for, under one name for code that
// runs on every platform - Windows::Error (Win32 codes) on Windows, Posix::Error (errno) elsewhere.
// The codes themselves differ between the platforms; compare them only through the names and
// predicates of Error.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Platform.Error;

#if defined(CT_PLATFORM_WINDOWS)
export import ThinkMeta.ConcurrentTasks.Windows.Error;
#else
export import ThinkMeta.ConcurrentTasks.Posix.Error;
#endif

namespace ThinkMeta::ConcurrentTasks::Platform {
#if defined(CT_PLATFORM_WINDOWS)
    namespace Native = Windows;
#else
    namespace Native = Posix;
#endif

    /// <summary>Names and predicates for the platform's error codes.</summary>
    /// <seealso cref="Windows::Error"/>
    export using Error = Native::Error;

    /// <summary>An error code of the platform; <see cref="Error::Success"/> on success.</summary>
    export using ErrorCode = Native::ErrorCode;

    /// <summary>Carries an error of the platform out of code that has no error return.</summary>
    /// <seealso cref="_ErrorException"/>
    export using ErrorException = Native::ErrorException;
} // namespace ThinkMeta::ConcurrentTasks::Platform
