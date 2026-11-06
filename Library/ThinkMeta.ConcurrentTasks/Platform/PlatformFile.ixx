//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// File: the file wrapper of the platform the library is built for, under one name for code that
// runs on every platform - Windows::File on Windows, Posix::File elsewhere. Portable code uses
// what both have: Create(fileName, FileOptions), Close, IsValid, GetSize, Flush and Delete, and
// FileName for paths (UTF-16 on Windows, UTF-8 elsewhere; CT_PATH in Core.h writes a literal).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Platform.File;

export import ThinkMeta.ConcurrentTasks.Platform.Error;
export import ThinkMeta.ConcurrentTasks.Platform.FileOptions;

#if defined(CT_PLATFORM_WINDOWS)
export import ThinkMeta.ConcurrentTasks.Windows.File;
#else
export import ThinkMeta.ConcurrentTasks.Posix.File;
#endif

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>Owns one open file of the platform.</summary>
    /// <seealso cref="Windows::File"/>
#if defined(CT_PLATFORM_WINDOWS)
    export using File = Windows::File;
#else
    export using File = Posix::File;
#endif

    /// <summary>A path as the platform takes it: <c>const wchar_t*</c> on Windows, <c>const char*</c> (UTF-8) elsewhere.</summary>
    export using FileName = File::FileName;
} // namespace ThinkMeta::ConcurrentTasks::Platform
