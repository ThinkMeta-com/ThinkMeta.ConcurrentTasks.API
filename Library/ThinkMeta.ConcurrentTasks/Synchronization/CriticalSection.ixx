//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// CriticalSection (any number of waiters) and CriticalSection_s (at most one waiter): a recursive
// mutex owned by a task, plus the scope guards CriticalSectionLock and SpinCriticalSectionLock.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include <atomic>
#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.CriticalSection;

import ThinkMeta.ConcurrentTasks.Synchronization.Suspendable;
import ThinkMeta.ConcurrentTasks.Cpp.Padded;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>A recursive mutual-exclusion lock owned by a task (not by a thread).</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of contending tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one suspended contender at a time, cheaper.
    /// </typeparam>
    /// <remarks>
    /// A task that holds it may enter again and must leave as often as it entered. A contending
    /// task is suspended, not blocked, so its thread runs other tasks. Comes as
    /// <see cref="CriticalSection"/> (any number of contending tasks) and
    /// <see cref="CriticalSection_s"/> (one contending task).
    /// All members identify the caller as the current task, so they must be called from a task or
    /// from an OS thread converted with <see cref="ConvertThreadToTask"/> - also
    /// <see cref="TryEnter"/> and <see cref="Leave"/>. Leave must be called by the owning task. On
    /// Leave the lock is handed directly to a suspended task (it becomes the owner before it runs);
    /// waiters are served LIFO, no fairness. No timeouts.
    /// CriticalSection_s: at most one task may be suspended in <see cref="Enter"/> at a time (the
    /// owner plus one contender).
    /// Destroy the critical section only when it is free and no task can enter or leave it any
    /// more.
    /// </remarks>
    /// <seealso cref="CriticalSectionLock"/>
    /// <seealso cref="SpinCriticalSectionLock"/>
    export template <SuspensionKind _SuspensionKind>
    class _CriticalSection : Suspendable<_SuspensionKind, _CriticalSection<_SuspensionKind>>
    {
    public:
        /// <summary>Creates a free critical section.</summary>
        _CriticalSection() noexcept = default;

        /// <summary>Enters if the lock is free or already held by the calling task.</summary>
        /// <returns>
        /// <c>true</c> if entered (lock count incremented); <c>false</c> if another task holds it.
        /// </returns>
        /// <remarks>Never suspends.</remarks>
        /// <seealso cref="Enter"/>
        /// <seealso cref="Leave"/>
        [[nodiscard]] bool TryEnter() noexcept
        {
            // relaxed is enough for the "already mine" check: only a hand-off to this very task
            // stores its handle, and that happens before the task is resumed.
            auto taskContext = GetCurrentTask();

            auto lockingTaskContext = lockingTaskContext_.load(std::memory_order_relaxed);
            if (taskContext != lockingTaskContext) {
                if (lockingTaskContext)
                    return false;

                TaskHandle expected = nullptr;
                if (!lockingTaskContext_.compare_exchange_strong(expected, taskContext, std::memory_order_acq_rel, std::memory_order_relaxed))
                    return false;
            }

            // lockCount_ is touched only by the owner. Taking a free lock, the acq_rel CAS above
            // orders it after the previous owner's release exchange in Leave; after a hand-off,
            // the order comes from the hand-off exchange followed by ResumeTask (Enter).
            ++lockCount_;
            return true;
        }

        /// <summary>Enters the lock, waiting for it if necessary.</summary>
        /// <remarks>
        /// Suspends the calling task while another task holds the lock. Recursive.
        /// <see cref="CriticalSection_s"/> may spin briefly (<c>YieldProcessor</c>) during a hand-off
        /// instead.
        /// </remarks>
        /// <seealso cref="TryEnter"/>
        /// <seealso cref="Leave"/>
        void Enter() noexcept
        {
            for (;;) {
                if (TryEnter())
                    return;

                if constexpr (_SuspensionKind == SuspensionKind::Single) {
                    // While the lock is in the transient hand-off state (the TaskHandle(1)
                    // sentinel published by GetNextLock), a pending waiter has not yet been
                    // promoted and the single waiter slot may still be occupied. Suspending
                    // here would overwrite and lose that waiter, so spin until the sentinel
                    // resolves to a real owner (then we suspend into an empty slot) or to
                    // null (then TryEnter acquires).
                    // Suspend only after seeing a real owner: if the sentinel has already
                    // resolved to null, TryEnter may have failed on it in both contenders
                    // (a late GetNextLock can set and clear it at any time), and both would
                    // suspend into the one slot.
                    if (auto observed = lockingTaskContext_.load(std::memory_order_relaxed);
                        observed == reinterpret_cast<TaskHandle>(1) || observed == nullptr) {
                        YieldProcessor();
                        continue;
                    }
                }

                // When Suspend returns, ResumeNext(lockingTaskContext_) has already made this task
                // the owner; only the count is left to do.
                this->Suspend();
                ++lockCount_;
                return;
            }
        }

        /// <summary>Leaves the lock once.</summary>
        /// <remarks>
        /// The last Leave hands the lock to a suspended task or frees it. Must be called by the
        /// owning task (asserted in Debug). Never suspends.
        /// </remarks>
        /// <seealso cref="Enter"/>
        void Leave() noexcept
        {
            // the owner stored its own handle, so a relaxed load sees it
            CT_ASSERT(lockingTaskContext_.load(std::memory_order_relaxed) == GetCurrentTask());

            if (!--lockCount_) {
                // hands the lock to the next waiter, or stores nullptr (free) if there is none;
                // a task that suspends right after that is picked up by GetNextLock
                if (!this->ResumeNext(lockingTaskContext_))
                    GetNextLock();
            }
        }

    private:
        // Called by Suspendable::OnSuspended: the lock may have become free between the failed
        // TryEnter and the publication of the waiter.
        void TryResume() noexcept { GetNextLock(); }

        void GetNextLock() noexcept
        {
            // Loop so that a task which suspends during the sentinel-claim / hand-off
            // window is not lost: after releasing we re-check for a pending waiter.
            // This works for both suspension kinds - for the single-waiter case the
            // loop exits immediately once the (only) waiter becomes the new owner.
            for (;;) {
                if (lockingTaskContext_.load(std::memory_order_relaxed) || !this->IsSuspended())
                    return;

                // Claim the free lock with the sentinel TaskHandle(1) (never a real handle), so
                // that no TryEnter can take it, then hand it to a waiter - or back to nullptr if
                // the waiter is gone - with ResumeNext.
                TaskHandle expected = nullptr;
                if (lockingTaskContext_.compare_exchange_strong(expected, reinterpret_cast<TaskHandle>(1), std::memory_order_acq_rel, std::memory_order_relaxed))
                    this->ResumeNext(lockingTaskContext_);
            }
        }

        // recursion depth of the owner; accessed by the owning task only
        std::size_t lockCount_{0};
        // the owning task, nullptr when free, TaskHandle(1) during a hand-off by GetNextLock
        CT_ALIGNED std::atomic<TaskHandle> lockingTaskContext_{nullptr};

        friend Suspendable<_SuspensionKind, _CriticalSection<_SuspensionKind>>;
    };

    /// <summary>Recursive task lock for any number of contending tasks.</summary>
    /// <seealso cref="CriticalSection_s"/>
    export using CriticalSection = _CriticalSection<SuspensionKind::Multiple>;

    /// <summary>Recursive task lock for at most one suspended contender at a time.</summary>
    /// <remarks>Cheaper than <see cref="CriticalSection"/>.</remarks>
    /// <seealso cref="CriticalSection"/>
    export using CriticalSection_s = _CriticalSection<SuspensionKind::Single>;

    /// <summary>Matches the critical section types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="CriticalSection"/> and <see cref="CriticalSection_s"/>.
    /// </remarks>
    /// <seealso cref="CriticalSection"/>
    /// <seealso cref="CriticalSection_s"/>
    export template <typename T>
    concept CriticalSectionType = std::same_as<T, CriticalSection> || std::same_as<T, CriticalSection_s>;

    /// <summary>Scope guard that holds a critical section.</summary>
    /// <typeparam name="_CriticalSectionType">
    /// The critical section type: <see cref="CriticalSection"/> or <see cref="CriticalSection_s"/>.
    /// </typeparam>
    /// <remarks>
    /// <see cref="_CriticalSection::Enter"/> on construction (may suspend the calling task),
    /// <see cref="_CriticalSection::Leave"/> on destruction. Task or converted thread only.
    /// </remarks>
    /// <seealso cref="SpinCriticalSectionLock"/>
    export template <CriticalSectionType _CriticalSectionType>
    class [[nodiscard("Discarding a lock guard immediately releases the lock.")]] CriticalSectionLock
    {
    public:
        /// <summary>Enters the critical section.</summary>
        /// <param name="criticalSection">The critical section; it must outlive the guard.</param>
        explicit CriticalSectionLock(_CriticalSectionType& criticalSection) noexcept : criticalSection_(criticalSection)
        {
            criticalSection_.Enter();
        }

        /// <summary>Leaves the critical section.</summary>
        ~CriticalSectionLock() noexcept { criticalSection_.Leave(); }

        CriticalSectionLock(const CriticalSectionLock&)            = delete;
        CriticalSectionLock(CriticalSectionLock&&)                 = delete;
        CriticalSectionLock& operator=(const CriticalSectionLock&) = delete;
        CriticalSectionLock& operator=(CriticalSectionLock&&)      = delete;

    private:
        _CriticalSectionType& criticalSection_;
    };

    /// <summary>Scope guard that holds a critical section, spinning before it suspends.</summary>
    /// <typeparam name="_CriticalSectionType">
    /// The critical section type: <see cref="CriticalSection"/> or <see cref="CriticalSection_s"/>.
    /// </typeparam>
    /// <remarks>
    /// Up to spinCount <see cref="_CriticalSection::TryEnter"/> attempts (with <c>YieldProcessor</c>
    /// in between, the thread is not given up), then a suspending
    /// <see cref="_CriticalSection::Enter"/>. <see cref="_CriticalSection::Leave"/> on destruction.
    /// Task or converted thread only.
    /// </remarks>
    /// <seealso cref="CriticalSectionLock"/>
    export template <CriticalSectionType _CriticalSectionType>
    class [[nodiscard("Discarding a lock guard immediately releases the lock.")]] SpinCriticalSectionLock
    {
    public:
        /// <summary>Enters the critical section, spinning first.</summary>
        /// <param name="criticalSection">The critical section; it must outlive the guard.</param>
        /// <param name="spinCount">
        /// The maximum number of spin attempts (<c>0</c>: <see cref="_CriticalSection::Enter"/> at once).
        /// </param>
        explicit SpinCriticalSectionLock(_CriticalSectionType& criticalSection, std::uint32_t spinCount) noexcept :
            criticalSection_(criticalSection)
        {
            // spinCount stays non-zero exactly when TryEnter succeeded
            while (spinCount) {
                if (criticalSection_.TryEnter())
                    break;

                YieldProcessor();
                --spinCount;
            }

            if (!spinCount)
                criticalSection_.Enter();
        }

        /// <summary>Leaves the critical section.</summary>
        ~SpinCriticalSectionLock() noexcept { criticalSection_.Leave(); }

        SpinCriticalSectionLock(const SpinCriticalSectionLock&)            = delete;
        SpinCriticalSectionLock(SpinCriticalSectionLock&&)                 = delete;
        SpinCriticalSectionLock& operator=(const SpinCriticalSectionLock&) = delete;
        SpinCriticalSectionLock& operator=(SpinCriticalSectionLock&&)      = delete;

    private:
        _CriticalSectionType& criticalSection_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Synchronization