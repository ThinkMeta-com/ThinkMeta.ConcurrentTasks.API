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
    /// </typeparam>
    /// <remarks>
    /// Wait tries <typeparamref name="T"/>::TryWait() first and suspends only if that fails; when a
    /// task suspends, TryWait() is checked again and T::Resume() is called if it now succeeds.
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
        /// calling task until a resumer lets it pass. Task or converted thread only; no timeout.
        /// </remarks>
        void Wait(this auto& self) noexcept
        {
            if (!self.TryWait())
                self.Suspend();
        }

        /// <summary>Waits with a time limit.</summary>
        /// <param name="timeout">
        /// The longest time to wait. Needs <see cref="EnableTimers"/>; without it the wait has no
        /// limit.
        /// </param>
        /// <returns>
        /// <c>true</c> if TryWait succeeded at once or a resumer let the task pass; <c>false</c> if
        /// it timed out, the task then no longer waits on the object.
        /// </returns>
        /// <remarks>Only for the single-waiter (_s) variants. Task or converted thread only.</remarks>
        /// <seealso cref="Duration"/>
        [[nodiscard]] bool Wait(this auto& self, Duration timeout) noexcept
            requires(_SuspensionKind == SuspensionKind::Single)
        {
            return self.TryWait() || self.SuspendFor(timeout);
        }

    private:
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

    /// <summary>Matches types that provide the waitable task interface.</summary>
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
} // namespace ThinkMeta::ConcurrentTasks