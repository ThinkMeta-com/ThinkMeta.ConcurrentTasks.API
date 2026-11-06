//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// VirtualMemory: reserved address space with pages committed and decommitted on demand, under one
// name for code that runs on every platform - Windows::VirtualMemory on Windows,
// Posix::VirtualMemory elsewhere. Portable code uses only what both have: Reserve, Release,
// IsValid, GetAllocationBase, AtOffset, Commit(address, size) and Decommit.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Platform.VirtualMemory;

export import ThinkMeta.ConcurrentTasks.Platform.Error;

#if defined(CT_PLATFORM_WINDOWS)
export import ThinkMeta.ConcurrentTasks.Windows.VirtualMemory;
#else
export import ThinkMeta.ConcurrentTasks.Posix.VirtualMemory;
#endif

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>Reserved address space with pages committed and decommitted on demand.</summary>
    /// <seealso cref="Windows::VirtualMemory"/>
#if defined(CT_PLATFORM_WINDOWS)
    export using VirtualMemory = Windows::VirtualMemory;
#else
    export using VirtualMemory = Posix::VirtualMemory;
#endif
} // namespace ThinkMeta::ConcurrentTasks::Platform
