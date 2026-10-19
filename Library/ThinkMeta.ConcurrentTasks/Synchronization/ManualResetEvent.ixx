//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// ManualResetEvent (any number of waiters) and ManualResetEvent_s (one waiter, timeouts): an
// event that stays set until Reset and lets every waiter pass, for tasks. ManualResetEvent_s is
// the completion event of the library's I/O requests.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <atomic>
#include <concepts>
#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

export import ThinkMeta.ConcurrentTasks.Synchronization.Waitable;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>A manual-reset event for tasks.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of waiting tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one waiting task at a time, cheaper, with timeouts.
    /// </typeparam>
    /// <remarks>
    /// While set, every <see cref="Wait"/> passes; <see cref="Set"/> resumes all waiting tasks.
    /// Comes as <see cref="ManualResetEvent"/> (any number of waiters) and
    /// <see cref="ManualResetEvent_s"/> (one waiter).
    /// Wait suspends the calling task and needs a task or an OS thread converted with
    /// <see cref="ConvertThreadToTask"/>. Set, <see cref="Reset"/> and <see cref="TryWait"/> never
    /// suspend and may be called from any thread (the core's I/O thread calls Set). No member
    /// reports errors.
    /// ManualResetEvent: any number of waiters, no timeouts. A waiter passes once it finds the event
    /// set after a resume; a Set taken back by a Reset before the waiter looked is not a pass, the
    /// waiter waits on. Destroy the event only when no task can wait on it or set it any more and
    /// the last Set has returned; the return of a Wait alone does not mean that: the Set that let
    /// the waiter pass may still be taking the list of waiters, and the waiter's own suspend
    /// callback may still be re-checking the event. A waiter that wants to destroy the event right
    /// after its wait uses ManualResetEvent_s, whose Set is one operation.
    /// </remarks>
    /// <seealso cref="AutoResetEvent"/>
    /// <seealso cref="SuspensionKind"/>
    export template <SuspensionKind _SuspensionKind>
    class _ManualResetEvent : public Waitable<_SuspensionKind, _ManualResetEvent<_SuspensionKind>>
    {
    public:
        /// <summary>A resumed waiter looks at the event again (see <see cref="Waitable"/>).</summary>
        /// <remarks>
        /// A resume alone does not let a waiter pass: the re-check after a suspension resumes every
        /// waiter on the list, among them tasks that started waiting after a Reset.
        /// </remarks>
        static constexpr bool WaitRechecks = true;

        /// <summary>Creates the event.</summary>
        /// <param name="isSet"><c>true</c> to create it set.</param>
        explicit _ManualResetEvent(bool isSet = false) noexcept : signal_(isSet ? 1 : 0) {}

        /// <summary>Sets the event and resumes every task suspended on it.</summary>
        /// <remarks>
        /// Never suspends; any thread. Takes the waiters in one step, after the signal is stored:
        /// the event must stay alive until Set has returned (see the class remarks).
        /// </remarks>
        /// <seealso cref="Reset"/>
        /// <seealso cref="Wait"/>
        void Set() noexcept
        {
            // seq_cst: the store must be visible before Resume() reads the list of waiters. A waiter adds
            // itself to that list and then reads the signal; with a release store both reads can miss
            // (store buffering), and the waiter stays suspended although the event is set.
            // A waiter that finds the signal set passes before the list is taken: the event must
            // still be alive then (see the class remarks).
            signal_.store(1, std::memory_order_seq_cst);
            this->Resume();
        }

        /// <summary>Clears the event.</summary>
        /// <remarks>Later <see cref="Wait"/> calls suspend again. Never suspends; any thread.</remarks>
        /// <seealso cref="Set"/>
        void Reset() noexcept { signal_.store(0, std::memory_order_release); }

        /// <summary>Tells whether the event is set, without changing it.</summary>
        /// <returns><c>true</c> if the event is set; otherwise <c>false</c>.</returns>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept { return signal_.load(std::memory_order_acquire) != 0; }

    private:
        // 0 reset, 1 set
        CT_ALIGNED std::atomic<std::size_t> signal_;
    };

    /// <summary>A manual-reset event for one waiting task, with timed waits.</summary>
    /// <remarks>
    /// This is <see cref="ManualResetEvent_s"/>. The waiter may destroy the event as soon as its
    /// <see cref="Wait"/> returned (or its <see cref="TryWait"/> returned <c>true</c>).
    /// At most one task may wait at a time (asserted in Debug).
    /// Destroying right after the wait covers the waiter and the <see cref="Set"/> that let it
    /// pass; any other Set, <see cref="Reset"/> or TryWait on the event must be finished by then.
    /// After a timed wait returned <c>false</c>, destroy the event only when no Set can come any
    /// more.
    /// Wait needs a task or a converted thread; Set, Reset and TryWait may be called from any
    /// thread and never suspend.
    /// </remarks>
    /// <seealso cref="ManualResetEvent"/>
    /// <seealso cref="AutoResetEvent_s"/>
    template <>
    class _ManualResetEvent<SuspensionKind::Single> : private ISuspendCallback
    {
        // State and waiter share one word, so that the operation that lets the waiter pass is the
        // last access to the event - by Set, by OnSuspended and by OnTimeout alike. The I/O
        // requests on a task's stack rely on this.

    public:
        /// <summary>Creates the event.</summary>
        /// <param name="isSet"><c>true</c> to create it set.</param>
        explicit _ManualResetEvent(bool isSet = false) noexcept : state_(isSet ? SetState : ResetState) {}

        /// <summary>Destroys the event.</summary>
        /// <remarks>No task may be waiting on it (asserted in Debug).</remarks>
        ~_ManualResetEvent() noexcept { CT_ASSERT(state_.load(std::memory_order_relaxed) <= SetState); } // no waiter left

        _ManualResetEvent(const _ManualResetEvent&)            = delete;
        _ManualResetEvent& operator=(const _ManualResetEvent&) = delete;

        /// <summary>Sets the event and resumes the waiting task, if any.</summary>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Reset"/>
        /// <seealso cref="Wait"/>
        void Set() noexcept
        {
            // sets the signal and takes the waiter in one operation, the last access
            const auto previous = state_.exchange(SetState, std::memory_order_acq_rel);
            if (previous > SetState)
                ResumeTask(ToTask(previous));
        }

        /// <summary>Clears the event.</summary>
        /// <remarks>A waiting task stays waiting. Never suspends; any thread.</remarks>
        /// <seealso cref="Set"/>
        void Reset() noexcept
        {
            // set -> reset only: a waiter that is published stays
            auto expected = SetState;
            state_.compare_exchange_strong(expected, ResetState, std::memory_order_acq_rel, std::memory_order_relaxed);
        }

        /// <summary>Tells whether the event is set, without changing it.</summary>
        /// <returns><c>true</c> if the event is set; otherwise <c>false</c>.</returns>
        /// <remarks>Never suspends; any thread.</remarks>
        /// <seealso cref="Wait"/>
        [[nodiscard]] bool TryWait() noexcept { return state_.load(std::memory_order_acquire) == SetState; }

        /// <summary>Waits until the event is set.</summary>
        /// <remarks>
        /// Returns at once if the event is set, otherwise suspends the calling task until
        /// <see cref="Set"/>. Task or converted thread only; at most one waiter at a time.
        /// </remarks>
        /// <seealso cref="TryWait"/>
        void Wait() noexcept
        {
            if (!TryWait())
                SuspendTask(this);
        }

        /// <summary>Waits until the event is set, with a time limit.</summary>
        /// <param name="timeout">
        /// The longest time to wait. Needs <see cref="EnableTimers"/>; without it the wait has no
        /// limit.
        /// </param>
        /// <returns>
        /// <c>true</c> if the event is set; <c>false</c> if it timed out: the task no longer waits on
        /// the event; the event may be set later.
        /// </returns>
        /// <remarks>Task or converted thread only.</remarks>
        /// <seealso cref="TryWait"/>
        [[nodiscard]] bool Wait(Duration timeout) noexcept { return TryWait() || SuspendTaskFor(this, timeout); }

    private:
        // ISuspendCallback: publishing and checking are one compare-exchange; when it succeeds, it is
        // the last access - only a Set or the timeout takes the waiter out again.
        void OnSuspended(SuspendedTask suspendedTask) noexcept override
        {
            auto expected = ResetState;
            if (state_.compare_exchange_strong(expected, FromTask(suspendedTask), std::memory_order_acq_rel, std::memory_order_acquire))
                return;
            CT_ASSERT(expected == SetState); // else a second task waits on a single-waiter event
            ResumeTask(suspendedTask);       // set already: resumed here, the event is not touched again
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

    /// <summary>Manual-reset event for any number of waiting tasks (no timeouts).</summary>
    /// <seealso cref="ManualResetEvent_s"/>
    export using ManualResetEvent = _ManualResetEvent<SuspensionKind::Multiple>;

    /// <summary>Manual-reset event for at most one waiting task.</summary>
    /// <remarks>Cheaper than <see cref="ManualResetEvent"/>, allows timeouts; the waiter may destroy it after its wait.</remarks>
    /// <seealso cref="ManualResetEvent"/>
    export using ManualResetEvent_s = _ManualResetEvent<SuspensionKind::Single>;

    /// <summary>Matches the manual-reset event types of this module.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <see cref="ManualResetEvent"/> and <see cref="ManualResetEvent_s"/>.
    /// </remarks>
    /// <seealso cref="ManualResetEvent"/>
    /// <seealso cref="ManualResetEvent_s"/>
    export template <typename T>
    concept ManualResetEventType = std::same_as<T, ManualResetEvent> || std::same_as<T, ManualResetEvent_s>;
} // namespace ThinkMeta::ConcurrentTasks