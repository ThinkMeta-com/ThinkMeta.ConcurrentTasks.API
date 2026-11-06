//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Thread: what the portable primitives need from the operating system's threads - giving up the
// rest of a time slice while they wait for another thread to finish a short step.
//
// The file is not called Thread.ixx: Visual Studio puts the objects of a project into one folder,
// where Windows\Thread.ixx already produces Thread.obj (MSB8027).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#else
#    include <sched.h>
#endif

export module ThinkMeta.ConcurrentTasks.Platform.Thread;

namespace ThinkMeta::ConcurrentTasks::Platform::Thread {
    /// <summary>Lets another thread run, if one is ready.</summary>
    /// <remarks>
    /// Gives up the rest of the calling thread's time slice and returns at once if no other thread
    /// is ready (<c>SwitchToThread</c> on Windows, <c>sched_yield</c> elsewhere). Not named Yield:
    /// that is a Windows macro.
    /// </remarks>
    export inline void YieldThread() noexcept
    {
#if defined(CT_PLATFORM_WINDOWS)
        SwitchToThread();
#else
        sched_yield();
#endif
    }
} // namespace ThinkMeta::ConcurrentTasks::Platform::Thread
