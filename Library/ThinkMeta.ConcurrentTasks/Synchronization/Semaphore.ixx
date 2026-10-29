//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Semaphore (any number of waiters) and Semaphore_s (one waiter, timeouts): a counting semaphore
// for tasks, plus the scope guards SemaphoreLock and SpinSemaphoreLock (each with an _s variant).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <Windows.h>

#include <atomic>
#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;

import ThinkMeta.ConcurrentTasks.Synchronization.Suspendable;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>A counting semaphore for tasks.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// <see cref="Wait"/> takes one permit, suspending the calling task while the count is
    /// <c>0</c>; <see cref="Release"/> adds permits and resumes as many waiters as it can serve.
    /// Comes as <see cref="Semaphore"/> (any number of waiters) and <see cref="Semaphore_s"/> (one
    /// waiter).
    /// Wait suspends and needs a task or an OS thread converted with
    /// <see cref="ConvertThreadToTask"/>. Release and <see cref="TryWait"/> never suspend and may be
    /// called from any thread; any task may release, not only one that waited. The count has no
    /// upper limit; keep it within the range of <c>std::size_t</c>. No member reports errors.
    /// Destroy the semaphore only when no task can wait on it or release it any more and the last
    /// Release has returned; the return of a Wait alone does not mean that (both variants).
    /// </remarks>
    /// <seealso cref="SemaphoreLock"/>
    /// <seealso cref="SpinSemaphoreLock"/>
    export template <SuspensionKind _SuspensionKind>
    class _Semaphore : protected Suspendable<_SuspensionKind, _Semaphore<_SuspensionKind>>
    {
    public:
        /// <summary>Creates the semaphore with a count of <c>0</c>.</summary>
        _Semaphore() noexcept : _Semaphore(0) {}

        /// <summary>Creates the semaphore with initial permits.</summary>
        /// <param name="initialCount">The number of permits available at once.</param>
        explicit _Semaphore(std::size_t initialCount) noexcept : count_(initialCount) {}

        /// <summary>Adds permits and resumes waiting tasks while permits last.</summary>
        /// <param name="count">The number of permits to add.</param>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        void Release(std::size_t count = 1) noexcept
        {
            // Publishes the releaser's writes to the task that takes the permit, and orders this add
            // before TryResume reads the waiter list: the signalling side of the pairing in
            // SuspendableBase<Single>::SetSuspendedTask, seq_cst on both sides (free on x64).
            count_.fetch_add(count, std::memory_order_seq_cst);
            TryResume();
        }

        /// <summary>Takes one permit, waiting for it if necessary.</summary>
        /// <remarks>
        /// Suspends the calling task until a <see cref="Release"/> hands it a permit. Task or
        /// converted thread only; <see cref="Semaphore_s"/>: at most one waiter at a time.
        /// </remarks>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept
        {
            if (!TryWait())
                this->Suspend();
        }

        /// <summary>Takes one permit, waiting for it with a time limit.</summary>
        /// <param name="timeout">
        /// The longest time to wait. Needs <see cref="EnableTimers"/>; without it the wait has no
        /// limit.
        /// </param>
        /// <returns><c>true</c> if a permit was taken; <c>false</c> if it timed out, no permit was taken then.</returns>
        /// <remarks><see cref="Semaphore_s"/> only. Task or converted thread only.</remarks>
        /// <seealso cref="TryWait"/>
        [[nodiscard]] bool Wait(Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            return TryWait() || this->SuspendFor(timeout);
        }

        /// <summary>Takes one permit if one is available.</summary>
        /// <returns><c>true</c> if a permit was taken; <c>false</c> if the count was <c>0</c>.</returns>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept
        {
            auto currentCount = count_.load(std::memory_order_acquire);
            for (;;) {
                if (!currentCount)
                    return false;

                auto expected = currentCount;
                if (count_.compare_exchange_strong(expected, currentCount - 1, std::memory_order_acq_rel, std::memory_order_acquire))
                    return true;

                currentCount = expected;
            }
        }

    private:
        // Called by Release and by Suspendable::OnSuspended. Takes a permit on behalf of a waiter
        // and then resumes one; if no waiter can be claimed any more (another resumer or the
        // timeout took it), the permit is given back and the loop checks again.
        void TryResume() noexcept
        {
            // loop while the semaphore does not block
            while (count_.load(std::memory_order_acquire)) {
                // exit loop if there aren't any blocked tasks
                if (!this->IsSuspended())
                    break;

                // exit loop if the semaphore is blocking
                if (!TryWait())
                    break;

                // resume a task or increment the release count (was decremented by TryWait() before)
                if (!this->ResumeNext())
                    count_.fetch_add(1, std::memory_order_release);
            }
        }

        CT_ALIGNED std::atomic<std::size_t> count_;
        friend Suspendable<_SuspensionKind, _Semaphore<_SuspensionKind>>;
    };

    /// <summary>Counting semaphore for any number of waiting tasks (no timeouts).</summary>
    /// <seealso cref="Semaphore_s"/>
    export using Semaphore = _Semaphore<SuspensionKind::Multiple>;

    /// <summary>Counting semaphore for at most one waiting task at a time.</summary>
    /// <remarks>
    /// Cheaper than <see cref="Semaphore"/> and allows timeouts.
    /// A waiter counts as waiting until it runs again, even after a Release has handed it its
    /// permit. Two tasks that take turns on one permit can therefore wait at the same time; as a
    /// lock between tasks use <see cref="CriticalSection_s"/> or <see cref="Semaphore"/> instead.
    /// </remarks>
    /// <seealso cref="Semaphore"/>
    export using Semaphore_s = _Semaphore<SuspensionKind::Single>;

    /// <summary>Matches the semaphore types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="Semaphore"/> and <see cref="Semaphore_s"/>.
    /// </remarks>
    /// <seealso cref="Semaphore"/>
    /// <seealso cref="Semaphore_s"/>
    export template <typename T>
    concept SemaphoreType = std::same_as<T, Semaphore> || std::same_as<T, Semaphore_s>;

    /// <summary>Scope guard that holds one permit of a semaphore.</summary>
    /// <typeparam name="_SemaphoreType">
    /// The semaphore type: <see cref="Semaphore"/> or <see cref="Semaphore_s"/>.
    /// </typeparam>
    /// <remarks>
    /// <see cref="_Semaphore::Wait"/> on construction (may suspend the calling task),
    /// <see cref="_Semaphore::Release"/> of one permit on destruction. Task or converted thread only.
    /// </remarks>
    /// <seealso cref="SpinSemaphoreLock"/>
    export template <SemaphoreType _SemaphoreType>
    class [[nodiscard("Discarding a lock guard immediately releases the lock.")]] SemaphoreLock
    {
    public:
        /// <summary>Takes one permit (<see cref="_Semaphore::Wait"/>).</summary>
        /// <param name="semaphore">The semaphore; it must outlive the guard.</param>
        explicit SemaphoreLock(_SemaphoreType& semaphore) noexcept : semaphore_(semaphore) { semaphore_.Wait(); }

        /// <summary>Releases the permit.</summary>
        ~SemaphoreLock() noexcept { semaphore_.Release(); }

        SemaphoreLock(const SemaphoreLock&)            = delete;
        SemaphoreLock(SemaphoreLock&&)                 = delete;
        SemaphoreLock& operator=(const SemaphoreLock&) = delete;
        SemaphoreLock& operator=(SemaphoreLock&&)      = delete;

    private:
        _SemaphoreType& semaphore_;
    };

    /// <summary>Scope guard that holds one permit of a semaphore, spinning before it suspends.</summary>
    /// <typeparam name="_SemaphoreType">
    /// The semaphore type: <see cref="Semaphore"/> or <see cref="Semaphore_s"/>.
    /// </typeparam>
    /// <remarks>
    /// Up to spinCount <see cref="_Semaphore::TryWait"/> attempts (with <c>YieldProcessor</c> in
    /// between, the thread is not given up), then a suspending <see cref="_Semaphore::Wait"/>.
    /// Releases one permit on destruction. Task or converted thread only.
    /// </remarks>
    /// <seealso cref="SemaphoreLock"/>
    export template <SemaphoreType _SemaphoreType>
    class [[nodiscard("Discarding a lock guard immediately releases the lock.")]] SpinSemaphoreLock
    {
    public:
        /// <summary>Takes one permit, spinning first.</summary>
        /// <param name="semaphore">The semaphore; it must outlive the guard.</param>
        /// <param name="spinCount">
        /// The maximum number of spin attempts (<c>0</c>: <see cref="_Semaphore::Wait"/> at once).
        /// </param>
        explicit SpinSemaphoreLock(_SemaphoreType& semaphore, std::uint32_t spinCount) noexcept : semaphore_(semaphore)
        {
            // spinCount stays non-zero exactly when TryWait succeeded
            while (spinCount) {
                if (semaphore_.TryWait())
                    break;

                YieldProcessor();
                --spinCount;
            }

            if (!spinCount)
                semaphore_.Wait();
        }

        /// <summary>Releases the permit.</summary>
        ~SpinSemaphoreLock() noexcept { semaphore_.Release(); }

        SpinSemaphoreLock(const SpinSemaphoreLock&)            = delete;
        SpinSemaphoreLock(SpinSemaphoreLock&&)                 = delete;
        SpinSemaphoreLock& operator=(const SpinSemaphoreLock&) = delete;
        SpinSemaphoreLock& operator=(SpinSemaphoreLock&&)      = delete;

    private:
        _SemaphoreType& semaphore_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Synchronization