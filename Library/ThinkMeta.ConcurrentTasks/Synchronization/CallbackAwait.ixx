//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Await: waits for an asynchronous operation that reports its completion through a callback, by
// suspending the calling task instead of blocking its thread. The building block of the Await
// functions for WinRT, Media Foundation and Windows handles.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <concepts>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Synchronization.Await;

import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::Synchronization {
    /// <summary>Completes a wait of <see cref="Await"/>: call it once when the operation has finished.</summary>
    /// <remarks>
    /// Small and copyable, so that it fits into the callback of any asynchronous API. Calling it is
    /// the last access to the waiting task's state; the waiting task may continue and return at once.
    /// </remarks>
    /// <seealso cref="Await"/>
    export class Completion
    {
    public:
        /// <summary>Wakes the waiting task.</summary>
        void operator()() const noexcept { completed_->Set(); }

    private:
        template <typename Start>
        friend void Await(Start&& start);

        explicit Completion(ManualResetEvent_s* completed) noexcept : completed_(completed) {}

        ManualResetEvent_s* completed_;
    };

    /// <summary>Starts an asynchronous operation and waits until it reports its completion.</summary>
    /// <typeparam name="Start">A callable that takes a <see cref="Completion"/> by reference.</typeparam>
    /// <param name="start">
    /// Starts the operation, or registers for its completion, and hands the <see cref="Completion"/>
    /// to the operation's callback, as a copy or by address: the referenced object stays valid until
    /// <c>Await</c> returns.
    /// </param>
    /// <remarks>
    /// Suspends the calling task until the completion is called; the scheduler thread runs other
    /// tasks meanwhile. The completion may be called before <paramref name="start"/> returns.
    /// It must be called exactly once, also when the operation fails or is canceled; the result and
    /// any error are fetched from the operation after the wait.
    /// Task or converted thread only. If <paramref name="start"/> throws, the completion must not be
    /// called any more.
    /// There is no timed variant: a timed-out wait would leave the callback behind with a reference
    /// to the task's stack. Limit the time with the operation itself, for example by canceling it.
    /// </remarks>
    /// <seealso cref="Completion"/>
    export template <typename Start>
    void Await(Start&& start)
    {
        static_assert(std::invocable<Start, Completion&>, "start must take a Completion&");

        // Set is the last access to the event, so it may live on the task's stack.
        ManualResetEvent_s completed;
        Completion         completion{&completed};
        start(completion);
        completed.Wait();
    }
} // namespace ThinkMeta::ConcurrentTasks::Synchronization
