//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// AutoResetEvent (any number of waiters) and AutoResetEvent_s (one waiter, timeouts): an event
// that lets exactly one waiter pass per Set and resets itself, for tasks.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <atomic>
#include <concepts>
#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

export import ThinkMeta.ConcurrentTasks.Synchronization.Waitable;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>An auto-reset event for tasks.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// <see cref="Set"/> lets one waiter pass (or, with no waiter, stays set until one
    /// <see cref="TryWait"/> / <see cref="Wait"/> takes the signal). Several Sets without a waiter
    /// in between count as one; with AutoResetEvent, Sets from several threads at the same moment
    /// may also count as one while tasks wait, so to count every signal use a
    /// <see cref="Semaphore"/>. Comes as <see cref="AutoResetEvent"/> (any number of waiters) and
    /// <see cref="AutoResetEvent_s"/> (one waiter).
    /// Wait suspends the calling task and needs a task or an OS thread converted with
    /// <see cref="ConvertThreadToTask"/>. Set, <see cref="Reset"/> and TryWait never suspend and
    /// may be called from any thread. No member reports errors.
    /// AutoResetEvent: any number of waiters, woken in LIFO order without fairness; no timeouts.
    /// Destroy the event only when no task can wait on it or set it any more; the return of a
    /// Wait alone does not mean that (AutoResetEvent_s allows destroying it right after the wait).
    /// </remarks>
    /// <seealso cref="ManualResetEvent"/>
    /// <seealso cref="SuspensionKind"/>
    export template <SuspensionKind _SuspensionKind>
    class _AutoResetEvent : public Waitable<_SuspensionKind, _AutoResetEvent<_SuspensionKind>>
    {
    public:
        /// <summary>Creates the event.</summary>
        /// <param name="isSet"><c>true</c> to create it set.</param>
        explicit _AutoResetEvent(bool isSet = false) noexcept : signal_(isSet ? 1 : 0) {}

        /// <summary>Sets the event.</summary>
        /// <remarks>
        /// Resumes one suspended waiter, which receives the signal (the event stays reset), or
        /// leaves the event set if no waiter is suspended. Never suspends.
        /// </remarks>
        /// <seealso cref="Reset"/>
        /// <seealso cref="Wait"/>
        void Set() noexcept
        {
            // A resumed waiter receives the signal, so the signal must be consumed before a
            // waiter is resumed; otherwise the waiter's next Wait() passes without a Set().
            // seq_cst: the store must be visible before IsSuspended() reads the waiter stack; a
            // waiter pushes itself and then reads the signal (see ManualResetEvent::Set).
            signal_.store(1, std::memory_order_seq_cst);

            while (this->IsSuspended()) {
                // a waiter's own TryResume() or another Set() took the signal and resumes for it
                if (!TryWait())
                    return;

                if (this->ResumeNext())
                    return;

                // the waiter was resumed by someone else: give the signal back and check again,
                // a waiter suspending from now on finds the signal in its own TryResume()
                signal_.store(1, std::memory_order_seq_cst);
            }
        }

        /// <summary>Clears the signal.</summary>
        /// <remarks>Suspended waiters stay suspended. Never suspends.</remarks>
        /// <seealso cref="Set"/>
        void Reset() noexcept { signal_.exchange(0, std::memory_order_acq_rel); }

        /// <summary>Takes the signal if the event is set.</summary>
        /// <returns>
        /// <c>true</c> if the signal was taken; the caller then counts as having passed a
        /// <see cref="Wait"/>. <c>false</c> if the event was not set.
        /// </returns>
        /// <remarks>The event is then reset. Never suspends.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept
        {
            std::size_t expected = 1;
            return signal_.compare_exchange_strong(expected, 0, std::memory_order_acquire, std::memory_order_relaxed);
        }

        /// <summary>Resumes one waiting task without touching the signal.</summary>
        /// <remarks>Part of the wait protocol; application code uses <see cref="Set"/> instead.</remarks>
        void Resume() noexcept
        {
            // Called by Waitable::TryResume after it took the signal for a waiter. That re-check
            // may come late: the waiter it stands for may already have been resumed by an earlier
            // Set, and the signal it took belongs to a later Set. If nobody is left to resume, the
            // signal goes back, exactly as in Set.
            if (!this->ResumeNext())
                Set();
        }

    private:
        // 0 reset, 1 set
        CT_ALIGNED std::atomic<std::size_t> signal_;
    };

    /// <summary>An auto-reset event for one waiting task, with timed waits.</summary>
    /// <remarks>
    /// This is <see cref="AutoResetEvent_s"/>. The waiter may destroy the event as soon as its
    /// <see cref="Wait"/> returned (or its <see cref="TryWait"/> returned <c>true</c>).
    /// At most one task may wait at a time (asserted in Debug).
    /// Destroying right after the wait covers the waiter and the <see cref="Set"/> that let it
    /// pass; any other Set, <see cref="Reset"/> or TryWait on the event must be finished by then.
    /// After a timed wait returned <c>false</c>, destroy the event only when no Set can come any
    /// more.
    /// Wait needs a task or a converted thread; Set, Reset and TryWait may be called from any
    /// thread and never suspend.
    /// </remarks>
    /// <seealso cref="AutoResetEvent"/>
    /// <seealso cref="ManualResetEvent_s"/>
    template <>
    class _AutoResetEvent<SuspensionKind::Single> : private ISuspendCallback
    {
        // State and waiter share one word, so that the operation that lets the waiter pass is the
        // last access to the event - by Set, by OnSuspended and by OnTimeout alike. There is no
        // second look after publishing the waiter, so a late OnSuspended cannot take a signal meant
        // for a later wait of the same task.

    public:
        /// <summary>Creates the event.</summary>
        /// <param name="isSet"><c>true</c> to create it set.</param>
        explicit _AutoResetEvent(bool isSet = false) noexcept : state_(isSet ? SetState : ResetState) {}

        /// <summary>Destroys the event.</summary>
        /// <remarks>No task may be waiting on it (asserted in Debug).</remarks>
        ~_AutoResetEvent() noexcept { CT_ASSERT(state_.load(std::memory_order_relaxed) <= SetState); } // no waiter left

        _AutoResetEvent(const _AutoResetEvent&)            = delete;
        _AutoResetEvent& operator=(const _AutoResetEvent&) = delete;

        /// <summary>Sets the event.</summary>
        /// <remarks>
        /// Resumes the waiting task, which receives the signal (the event stays reset), or sets the
        /// event if no task waits. Setting a set event does nothing. Never suspends; any thread.
        /// </remarks>
        /// <seealso cref="Reset"/>
        /// <seealso cref="Wait"/>
        void Set() noexcept
        {
            auto state = state_.load(std::memory_order_acquire);
            for (;;) {
                if (state == SetState)
                    return; // set already: one signal is enough
                if (state == ResetState) {
                    if (state_.compare_exchange_weak(state, SetState, std::memory_order_acq_rel, std::memory_order_acquire))
                        return; // no waiter: the signal stays
                    continue;
                }
                // a waiter: taken out, it receives the signal and the event stays reset
                if (state_.compare_exchange_weak(state, ResetState, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    ResumeTask(ToTask(state)); // only the local handle from here
                    return;
                }
            }
        }

        /// <summary>Clears the signal.</summary>
        /// <remarks>A waiting task stays waiting. Never suspends; any thread.</remarks>
        /// <seealso cref="Set"/>
        void Reset() noexcept
        {
            // set -> reset only: a waiter that is published stays
            auto expected = SetState;
            state_.compare_exchange_strong(expected, ResetState, std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        /// <summary>Takes the signal if the event is set.</summary>
        /// <returns>
        /// <c>true</c> if the signal was taken (the event is reset now); <c>false</c> if the event was not
        /// set.
        /// </returns>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept
        {
            auto expected = SetState;
            return state_.compare_exchange_strong(expected, ResetState, std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        /// <summary>Waits for the signal and takes it.</summary>
        /// <remarks>
        /// Suspends the calling task until a <see cref="Set"/> if the event is reset. Task or
        /// converted thread only; at most one waiter at a time.
        /// </remarks>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept
        {
            if (!TryWait())
                SuspendTask(this);
        }

        /// <summary>Waits for the signal with a time limit.</summary>
        /// <param name="timeout">
        /// The longest time to wait. Needs <see cref="EnableTimers"/>; without it the wait has no
        /// limit.
        /// </param>
        /// <returns>
        /// <c>true</c> if the signal was taken; <c>false</c> if it timed out: the signal was not taken
        /// and the task no longer waits on the event.
        /// </returns>
        /// <remarks>Task or converted thread only.</remarks>
        /// <seealso cref="TryWait"/>
        [[nodiscard]] bool Wait(Duration timeout) noexcept { return TryWait() || SuspendTaskFor(this, timeout); }

    private:
        // ISuspendCallback: publishing and checking are one compare-exchange; when it succeeds, it is
        // the last access - only a Set or the timeout takes the waiter out again. When the event is
        // set, the signal is taken for this very task, which nobody else can have resumed.
        void OnSuspended(SuspendedTask suspendedTask) noexcept override
        {
            for (;;) {
                auto expected = ResetState;
                if (state_.compare_exchange_strong(expected, FromTask(suspendedTask), std::memory_order_acq_rel, std::memory_order_acquire))
                    return;
                CT_ASSERT(expected == SetState); // else a second task waits on a single-waiter event
                expected = SetState;
                if (state_.compare_exchange_strong(expected, ResetState, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    ResumeTask(suspendedTask); // the signal taken for this task; the event is not touched again
                    return;
                }
                // another task's TryWait took the signal: reset again, publish once more
            }
        }

        // ISuspendCallback: the timeout and Set race for the waiter in the word; whoever takes it out
        // resumes it (while this runs, the core keeps a concurrent resume pending).
        bool OnTimeout(SuspendedTask suspendedTask) noexcept override
        {
            auto expected = FromTask(suspendedTask);
            return state_.compare_exchange_strong(expected, ResetState, std::memory_order_acq_rel, std::memory_order_acquire);
        }

        // 0 reset, 1 set, otherwise the waiting task (reset): a task handle is an aligned SLIST_ENTRY
        // and never 0 or 1.
        static constexpr std::uintptr_t ResetState = 0;
        static constexpr std::uintptr_t SetState   = 1;

        static std::uintptr_t FromTask(SuspendedTask task) noexcept { return reinterpret_cast<std::uintptr_t>(task); }
        static SuspendedTask ToTask(std::uintptr_t state) noexcept { return reinterpret_cast<SuspendedTask>(state); }

        CT_ALIGNED std::atomic<std::uintptr_t> state_;
    };

    /// <summary>Auto-reset event for any number of waiting tasks (no timeouts).</summary>
    /// <seealso cref="AutoResetEvent_s"/>
    export using AutoResetEvent = _AutoResetEvent<SuspensionKind::Multiple>;

    /// <summary>Auto-reset event for at most one waiting task.</summary>
    /// <remarks>Cheaper than <see cref="AutoResetEvent"/>, allows timeouts; the waiter may destroy it after its wait.</remarks>
    /// <seealso cref="AutoResetEvent"/>
    export using AutoResetEvent_s = _AutoResetEvent<SuspensionKind::Single>;

    /// <summary>Matches the auto-reset event types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="AutoResetEvent"/> and <see cref="AutoResetEvent_s"/>.
    /// </remarks>
    /// <seealso cref="AutoResetEvent"/>
    /// <seealso cref="AutoResetEvent_s"/>
    export template <typename T>
    concept AutoResetEventType = std::same_as<T, AutoResetEvent> || std::same_as<T, AutoResetEvent_s>;
} // namespace ThinkMeta::ConcurrentTasks::Synchronization