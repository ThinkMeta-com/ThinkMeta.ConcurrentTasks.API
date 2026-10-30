//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Await: waits for a C++/WinRT asynchronous operation by suspending the calling task instead of
// blocking its thread. The module needs no WinRT headers; the caller includes C++/WinRT.
//
// Preliminary API: everything in this file is subject to change.
//
module;

#include <concepts>
#include <type_traits>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.WinRT.Await;

import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;

namespace ThinkMeta::ConcurrentTasks::WinRT {
    /// <summary>Matches the C++/WinRT asynchronous operation types.</summary>
    /// <typeparam name="T">The candidate type.</typeparam>
    /// <remarks>
    /// Satisfied by <c>IAsyncAction</c>, <c>IAsyncOperation&lt;TResult&gt;</c>,
    /// <c>IAsyncActionWithProgress&lt;TProgress&gt;</c> and
    /// <c>IAsyncOperationWithProgress&lt;TResult, TProgress&gt;</c>, and by every type with the same
    /// <c>Status</c>, <c>Completed</c> and <c>GetResults</c> members.
    /// </remarks>
    /// <seealso cref="Await"/>
    export template <typename T>
    concept AsyncOperationType = requires(const T& op) {
        requires std::is_enum_v<decltype(op.Status())>; // AsyncStatus
        op.Completed([](auto&&, auto&&) noexcept {});
        op.GetResults();
    };

    /// <summary>Waits for an asynchronous WinRT operation and returns its result.</summary>
    /// <typeparam name="AsyncOp">The operation type, see <see cref="AsyncOperationType"/>.</typeparam>
    /// <param name="op">The started operation.</param>
    /// <returns>The result of <c>GetResults</c>; nothing for an action.</returns>
    /// <remarks>
    /// Returns at once if the operation has finished, otherwise suspends the calling task until the
    /// operation completes; the scheduler thread runs other tasks meanwhile. Throws like
    /// <c>co_await</c>: <c>GetResults</c> throws <c>winrt::hresult_error</c> for a failed operation
    /// and <c>winrt::hresult_canceled</c> for a canceled one.
    /// Task or converted thread only. Sets the operation's <c>Completed</c> handler, which an
    /// operation accepts only once.
    /// The task may resume on another scheduler thread: use agile objects only (the WinRT default;
    /// not objects bound to a single-threaded apartment). Initialize the multithreaded apartment
    /// once for the process, e.g. <c>winrt::init_apartment()</c> on the main thread, so that the
    /// scheduler threads belong to it implicitly.
    /// There is no timed variant: a timed-out wait would leave the handler behind with a reference
    /// to the task's stack.
    /// </remarks>
    /// <seealso cref="AsyncOperationType"/>
    export template <AsyncOperationType AsyncOp>
    auto Await(const AsyncOp& op)
    {
        // AsyncStatus::Started is 0; every other status means the operation has finished.
        if (op.Status() == decltype(op.Status()){}) {
            // Set is the last access to the event, so it may live on the task's stack. An operation
            // that completes meanwhile calls the handler at once, and Wait passes.
            Synchronization::ManualResetEvent_s completed;
            op.Completed([&completed](auto&&, auto&&) noexcept { completed.Set(); });
            completed.Wait();
        }

        return op.GetResults();
    }
} // namespace ThinkMeta::ConcurrentTasks::WinRT
