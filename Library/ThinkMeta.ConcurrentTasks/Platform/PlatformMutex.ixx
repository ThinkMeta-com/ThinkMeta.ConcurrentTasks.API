//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Mutex: a plain lock of the operating system for short sections that threads (not tasks) may
// contend for - an SRWLOCK on Windows, a pthread mutex elsewhere. A task that takes it blocks its
// scheduler thread while it waits; use Synchronization::CriticalSection between tasks.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#else
#    include <pthread.h>
#endif

export module ThinkMeta.ConcurrentTasks.Platform.Mutex;

namespace ThinkMeta::ConcurrentTasks::Platform {
    /// <summary>A lock of the operating system, held for short sections.</summary>
    /// <remarks>
    /// Not recursive. Waiting blocks the thread, also a scheduler thread that runs a task: for
    /// sections shared between tasks use <see cref="Synchronization::CriticalSection"/>.
    /// Needs no initialization and no clean-up.
    /// </remarks>
    /// <seealso cref="MutexGuard"/>
    export class Mutex
    {
    public:
        /// <summary>Constructs an unlocked mutex.</summary>
        constexpr Mutex() noexcept = default;

        Mutex(const Mutex&)            = delete;
        Mutex& operator=(const Mutex&) = delete;

        /// <summary>Takes the lock, waiting while another thread holds it.</summary>
        void Lock() noexcept
        {
#if defined(CT_PLATFORM_WINDOWS)
            AcquireSRWLockExclusive(&lock_);
#else
            pthread_mutex_lock(&lock_);
#endif
        }

        /// <summary>Releases the lock taken by <see cref="Lock"/>.</summary>
        void Unlock() noexcept
        {
#if defined(CT_PLATFORM_WINDOWS)
            ReleaseSRWLockExclusive(&lock_);
#else
            pthread_mutex_unlock(&lock_);
#endif
        }

    private:
#if defined(CT_PLATFORM_WINDOWS)
        SRWLOCK lock_ = SRWLOCK_INIT;
#else
        pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
#endif
    };

    /// <summary>Holds a <see cref="Mutex"/> for a scope.</summary>
    export class MutexGuard
    {
    public:
        /// <summary>Takes <paramref name="mutex"/>.</summary>
        /// <param name="mutex">The mutex to hold until the guard ends.</param>
        explicit MutexGuard(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.Lock(); }

        /// <summary>Releases the mutex.</summary>
        ~MutexGuard() noexcept { mutex_.Unlock(); }

        MutexGuard(const MutexGuard&)            = delete;
        MutexGuard& operator=(const MutexGuard&) = delete;

    private:
        Mutex& mutex_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Platform
