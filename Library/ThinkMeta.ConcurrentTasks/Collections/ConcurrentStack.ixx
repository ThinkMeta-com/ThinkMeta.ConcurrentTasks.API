//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// ConcurrentStack: a lock-free intrusive LIFO list over the Windows interlocked SList
// (InterlockedPushEntrySList / InterlockedPopEntrySList), for SLIST_ENTRY-derived items.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <type_traits>

export module ThinkMeta.ConcurrentTasks.Collections.ConcurrentStack;

namespace ThinkMeta::ConcurrentTasks::Collections {
    /// <summary>A lock-free intrusive LIFO stack.</summary>
    /// <typeparam name="T">
    /// The item type; derives from <c>SLIST_ENTRY</c>, through which items are linked, and is aligned
    /// to <c>MEMORY_ALLOCATION_ALIGNMENT</c>.
    /// </typeparam>
    /// <remarks>
    /// <see cref="Push"/> and <see cref="Pop"/> may be called from any number of threads and tasks
    /// at once. Items are linked through their <c>SLIST_ENTRY</c> base, so pushing never allocates.
    /// Requirements of the Windows SList apply: every item must be aligned to
    /// <c>MEMORY_ALLOCATION_ALIGNMENT</c> (16 bytes on x64), as must the stack object itself. The stack
    /// does not own its items; an item must stay alive while it is on the stack. Destroying a
    /// non-empty stack just forgets the items.
    /// </remarks>
    /// <seealso cref="BasicStack"/>
    export template <typename T>
    requires std::is_base_of_v<SLIST_ENTRY, T>
    class ConcurrentStack
    {
    public:
        /// <summary>Constructs an empty stack.</summary>
        ConcurrentStack() noexcept { InitializeSListHead(&head_); }

        ConcurrentStack(const ConcurrentStack&)            = delete;
        ConcurrentStack(ConcurrentStack&&)                 = delete;
        ConcurrentStack& operator=(const ConcurrentStack&) = delete;
        ConcurrentStack& operator=(ConcurrentStack&&)      = delete;

        /// <summary>Pushes an item on top.</summary>
        /// <param name="item">Not <c>nullptr</c>; its <c>SLIST_ENTRY::Next</c> is overwritten.</param>
        /// <seealso cref="Pop"/>
        void Push(T* item) noexcept { InterlockedPushEntrySList(&head_, item); }

        /// <summary>Removes and returns the top item.</summary>
        /// <returns>The top item, or <c>nullptr</c> if the stack is empty.</returns>
        /// <seealso cref="Push"/>
        T* Pop() noexcept { return static_cast<T*>(InterlockedPopEntrySList(&head_)); }

        /// <summary>Returns the number of items at the moment of the call.</summary>
        /// <returns>The item count, modulo 65536.</returns>
        /// <remarks>
        /// Uses <c>QueryDepthSList</c>; with concurrent <see cref="Push"/> and <see cref="Pop"/> the
        /// result may be outdated at once. Counts above 65535 wrap (16 bits).
        /// </remarks>
        unsigned short Size() const noexcept { return QueryDepthSList(&head_); }

    private:
        // mutable because QueryDepthSList takes a non-const header even though it only reads.
        mutable SLIST_HEADER head_;
    };
} // namespace ThinkMeta::ConcurrentTasks::Collections
