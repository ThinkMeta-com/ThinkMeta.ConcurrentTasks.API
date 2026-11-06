//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Clock: the monotonic clock of the platform in units of 100 ns, the unit of Duration, so that a
// deadline is the sum of Now and a duration. Windows: the performance counter, which counts at
// 10 MHz on current Windows and is converted otherwise. Linux: CLOCK_MONOTONIC.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#else
#    include <time.h>
#endif

#include <cstdint>

export module ThinkMeta.ConcurrentTasks.Platform.Clock;

namespace ThinkMeta::ConcurrentTasks::Platform::Clock {
    /// <summary>Returns the time in units of 100 ns since an arbitrary start.</summary>
    /// <returns>A monotonic reading: later calls never return less.</returns>
    /// <remarks>
    /// The same unit as <see cref="Duration"/>, so <c>Now() + duration.hundredNanoseconds</c> is a
    /// deadline to compare later readings with. The start is arbitrary; only differences mean
    /// anything.
    /// </remarks>
    export inline std::int64_t Now() noexcept
    {
#if defined(CT_PLATFORM_WINDOWS)
        LARGE_INTEGER counter;
        LARGE_INTEGER frequency;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart == 10000000)
            return counter.QuadPart;
        return counter.QuadPart / frequency.QuadPart * 10000000 + counter.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
#else
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        return static_cast<std::int64_t>(now.tv_sec) * 10000000 + now.tv_nsec / 100;
#endif
    }
} // namespace ThinkMeta::ConcurrentTasks::Platform::Clock
