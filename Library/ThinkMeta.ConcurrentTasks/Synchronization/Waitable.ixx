//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Waitable: the Wait() / Wait(timeout) front end shared by the primitives whose condition is a
// TryWait() (the Multiple events and TaskGroup / TaskGroup_s).
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.Waitable;

export import ThinkMeta.ConcurrentTasks.Synchronization.Suspendable;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>Turns a non-blocking TryWait() into a suspending <see cref="Wait"/>().</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, with timeouts.
    /// </typeparam>
    /// <typeparam name="T">
    /// The primitive built on it; provides a public bool TryWait() (non-blocking, may consume the
    /// signal) and a Resume() that resumes the suspended task(s) the passed TryWait stands for.
    /// A T whose condition stays observable after a resume (a manual-reset event, a task group's
    /// count) declares <c>static constexpr bool WaitRechecks = true</c>: its Wait then checks
    /// TryWait() again after every resume and suspends again if the condition no longer holds. A T
    /// whose resume hands the signal to the resumed task (an auto-reset event, a semaphore) does
    /// not, since its TryWait() would find nothing after a legitimate resume.
    /// </typeparam>
    /// <remarks>
    /// Wait tries <typeparamref name="T"/>::TryWait() first and suspends only if that fails; when a
    /// task suspends, TryWait() is checked again and T::Resume() is called if it now succeeds.
    /// That re-check resumes every task suspended at that moment, so with
    /// <see cref="SuspensionKind::Multiple"/> a task can be resumed although the condition was
    /// taken back meanwhile (a Reset between a Set and the re-check); a T with WaitRechecks
    /// absorbs that by waiting again. With <see cref="SuspensionKind::Single"/> the same happens
    /// when the waiter reuses the object (a TaskGroup_s of the next round) while the re-check of
    /// its previous wait is still running.
    /// Wait must be called from a task or from an OS thread converted with
    /// <see cref="ConvertThreadToTask"/>; it suspends the caller if TryWait fails. For
    /// <see cref="SuspensionKind::Single"/>, at most one task may wait at a time.
    /// </remarks>
    /// <seealso cref="Suspendable"/>
    export template <SuspensionKind _SuspensionKind, class T>
    class CT_NOVTABLE Waitable : protected Suspendable<_SuspensionKind, Waitable<_SuspensionKind, T>>
    {
    public:
        /// <summary>Waits until the primitive lets the calling task pass.</summary>
        /// <remarks>
        /// Returns at once if <typeparamref name="T"/>::TryWait() succeeds, otherwise suspends the
        /// calling task until a resumer lets it pass (and, with WaitRechecks, until TryWait() holds
        /// after the resume). Task or converted thread only; no timeout.
        /// </remarks>
        void Wait(this auto& self) noexcept
        {
            while (!self.TryWait()) {
                self.Suspend();
                if constexpr (!Rechecks)
                    break;
            }
        }

        /// <summary>Waits with a time limit.</summary>
        /// <param name="timeout">
        /// The longest time to wait. Needs <see cref="EnableTimers"/>; without it the wait has no
        /// limit.
        /// </param>
        /// <returns>
        /// <c>true</c> if TryWait succeeded at once or a resumer let the task pass (with
        /// WaitRechecks: and TryWait() held after the resume); <c>false</c> if it timed out, the
        /// task then no longer waits on the object.
        /// </returns>
        /// <remarks>Only for the single-waiter (_s) variants. Task or converted thread only.</remarks>
        /// <seealso cref="Duration"/>
        [[nodiscard]] bool Wait(this auto& self, Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            if constexpr (!Rechecks)
                return self.TryWait() || self.SuspendFor(timeout);
            else {
                if (self.TryWait())
                    return true;

                // A resume without the condition waits on for the time that is left.
                const long long deadline = timeout.IsInfinite() ? 0 : Deadline(timeout);
                for (;;) {
                    if (!self.SuspendFor(timeout))
                        return false;
                    if (self.TryWait())
                        return true;
                    if (!timeout.IsInfinite()) {
                        const long long left = deadline - Now();
                        if (left <= 0)
                            return false;
                        timeout = Duration{left};
                    }
                }
            }
        }

    private:
        // Whether T declares WaitRechecks as true (see the class remarks).
        static constexpr bool Rechecks = requires { requires T::WaitRechecks; };

        // The clock of a timed wait that waits again, in 100 ns units. Only on the rare path after
        // a resume without the condition, so the two QPC calls do not matter.
        static long long Now() noexcept
        {
            LARGE_INTEGER counter;
            LARGE_INTEGER frequency;
            QueryPerformanceCounter(&counter);
            QueryPerformanceFrequency(&frequency);
            return counter.QuadPart / frequency.QuadPart * 10000000 + counter.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
        }

        static long long Deadline(Duration timeout) noexcept
        {
            const long long now = Now();
            return timeout.hundredNanoseconds < Duration::Infinite().hundredNanoseconds - now ? now + timeout.hundredNanoseconds
                                                                                                : Duration::Infinite().hundredNanoseconds;
        }

        // Called by Suspendable::OnSuspended after the task was published: a signal that came
        // between the failed TryWait in Wait and the publication would otherwise be missed.
        // If TryWait consumes a signal here (AutoResetEvent) and the published task was already
        // resumed by someone else, T::Resume() may find nobody to resume; it must then give the
        // signal back.
        void TryResume() noexcept
        {
            if (TryWait())
                static_cast<T*>(this)->Resume();
        }

        // forwards to T::TryWait (which hides this one for users of T)
        bool TryWait() noexcept { return static_cast<T*>(this)->TryWait(); }

        friend Suspendable<_SuspensionKind, Waitable<_SuspensionKind, T>>;
    };

    /// <summary>Matches types that provide the waitable interface.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    export template <typename T>
    concept WaitableType = requires(T& w) {
        { w.Wait() } noexcept;
        { w.TryWait() } noexcept -> std::same_as<bool>;
    };

    /// <summary>Matches types that provide the waitable task interface with timeouts.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    export template <typename T>
    concept TimedWaitableType = WaitableType<T> && requires(T& w, Duration timeout) {
        { w.Wait(timeout) } noexcept -> std::same_as<bool>;
    };
} // namespace ThinkMeta::ConcurrentTasks::Synchronization