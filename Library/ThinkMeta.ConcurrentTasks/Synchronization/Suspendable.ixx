//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Suspendable: SuspendableBase plus the OnSuspended callback that publishes the suspended task
// and then lets the derived primitive re-check its condition (T::TryResume), so that a signal
// arriving while the task suspends is not lost.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <ThinkMeta.ConcurrentTasks/Core.h>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Synchronization.Suspendable;

export import ThinkMeta.ConcurrentTasks.Synchronization.SuspendableBase;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>Records a suspending task as waiting and then re-checks the primitive's condition.</summary>
    /// <typeparam name="_SuspensionKind">
    /// <see cref="SuspensionKind::Multiple"/>: any number of suspended tasks;
    /// <see cref="SuspensionKind::Single"/>: at most one suspended task at a time.
    /// </typeparam>
    /// <typeparam name="T">
    /// The primitive built on it. Its TryResume() checks the condition and resumes a suspended
    /// task if the condition holds.
    /// </typeparam>
    /// <remarks>
    /// Building block of the synchronization primitives: the re-check
    /// (<typeparamref name="T"/>::TryResume) makes sure that a signal arriving while the task
    /// suspends is not lost.
    /// The re-check runs once the task is off the processor - on a scheduler thread, or on the
    /// converted OS thread itself before it blocks - and may run after another thread has already
    /// resumed the task. TryResume therefore must not assume that the task it just recorded is
    /// still suspended.
    /// </remarks>
    /// <seealso cref="SuspendableBase"/>
    /// <seealso cref="Waitable"/>
    export template <SuspensionKind _SuspensionKind, typename T>
    class CT_NOVTABLE Suspendable;

    template <typename T>
    class CT_NOVTABLE Suspendable<SuspensionKind::Multiple, T> : public SuspendableBase<SuspensionKind::Multiple>
    {
    private:
        // ISuspendCallback: push first, then re-check (see SuspendableBase<Single>::SetSuspendedTask
        // for the ordering argument; here the push is an SList interlocked operation). The re-check
        // reads the object after the task was published, when a resumer may already have let the
        // task pass: the object must still be alive then, which is why a Multiple object may not be
        // destroyed right after a wait (TaskGroup covers this case with its notification count).
        void OnSuspended(SuspendedTask suspendedTask) noexcept override
        {
            AddSuspendedTask(suspendedTask);
            static_cast<T*>(this)->TryResume();
        }
    };

    template <typename T>
    class CT_NOVTABLE Suspendable<SuspensionKind::Single, T> : public SuspendableBase<SuspensionKind::Single>
    {
    private:
        // ISuspendCallback: publish into the slot first, then re-check. Unlike the "_s" events,
        // this is a second look at the object after the task was published: a resumer may already
        // have taken the task, and the object must still be alive when TryResume runs.
        void OnSuspended(SuspendedTask suspendedTask) noexcept override
        {
            SetSuspendedTask(suspendedTask);
            static_cast<T*>(this)->TryResume();
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Synchronization
