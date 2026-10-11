//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// BasicStack: an intrusive, unsynchronized LIFO list of SLIST_ENTRY-derived items. Used by
// synchronization primitives under their own lock, e.g. to chain suspended tasks
// (SuspendedTaskEntry in Core.h).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>
#include <type_traits>

export module ThinkMeta.ConcurrentTasks.Collections.BasicStack;

namespace ThinkMeta::ConcurrentTasks::Collections {
    /// <summary>An intrusive, unsynchronized LIFO stack.</summary>
    /// <typeparam name="T">The item type; derives from <c>SLIST_ENTRY</c>, through which items are linked.</typeparam>
    /// <remarks>
    /// Items are linked through their <c>SLIST_ENTRY</c> base, so pushing never allocates. Not
    /// thread-safe; see <see cref="ConcurrentStack"/> for the lock-free variant.
    /// The stack does not own its items: an item must stay alive and must not be pushed onto any
    /// other list while it is on the stack. Destroying a non-empty stack just forgets the items.
    /// </remarks>
    /// <seealso cref="ConcurrentStack"/>
    export template <typename T>
    requires std::is_base_of_v<SLIST_ENTRY, T>
    class BasicStack
    {
    public:
        /// <summary>Constructs an empty stack.</summary>
        BasicStack() noexcept = default;

        BasicStack(const BasicStack&)            = delete;
        BasicStack(BasicStack&&)                 = delete;
        BasicStack& operator=(const BasicStack&) = delete;
        BasicStack& operator=(BasicStack&&)      = delete;

        /// <summary>Pushes an item on top.</summary>
        /// <param name="item">Not <c>nullptr</c>; its <c>SLIST_ENTRY::Next</c> is overwritten.</param>
        /// <seealso cref="Pop"/>
        void Push(T* item) noexcept
        {
            item->Next = head_;
            head_      = item;
        }

        /// <summary>Removes and returns the top item.</summary>
        /// <returns>The top item, or <c>nullptr</c> if the stack is empty.</returns>
        /// <seealso cref="Push"/>
        T* Pop() noexcept
        {
            auto result = head_;

            if (result)
                head_ = static_cast<T*>(head_->Next);

            return result;
        }

    private:
        T* head_ = nullptr;
    };
} // namespace ThinkMeta::ConcurrentTasks::Collections
