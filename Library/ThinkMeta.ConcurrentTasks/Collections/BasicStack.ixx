//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// BasicStack: an intrusive, unsynchronized LIFO list of ListEntry-derived items. Used by
// synchronization primitives under their own lock, e.g. to chain suspended tasks
// (SuspendedTaskEntry in Core.h).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <type_traits>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Collections.BasicStack;

namespace ThinkMeta::ConcurrentTasks::Collections {
    /// <summary>An intrusive, unsynchronized LIFO stack.</summary>
    /// <typeparam name="T">The item type; derives from <see cref="ListEntry"/>, through which items are linked.</typeparam>
    /// <remarks>
    /// Items are linked through their <see cref="ListEntry"/> base, so pushing never allocates. Not
    /// thread-safe; see <see cref="ConcurrentStack"/> for the lock-free variant.
    /// The stack does not own its items: an item must stay alive and must not be pushed onto any
    /// other list while it is on the stack. Destroying a non-empty stack just forgets the items.
    /// </remarks>
    /// <seealso cref="ConcurrentStack"/>
    export template <typename T>
    requires std::is_base_of_v<ListEntry, T>
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
        /// <param name="item">Not <c>nullptr</c>; its <see cref="ListEntry::next"/> is overwritten.</param>
        /// <seealso cref="Pop"/>
        void Push(T* item) noexcept
        {
            item->next = head_;
            head_      = item;
        }

        /// <summary>Removes and returns the top item.</summary>
        /// <returns>The top item, or <c>nullptr</c> if the stack is empty.</returns>
        /// <seealso cref="Push"/>
        T* Pop() noexcept
        {
            auto result = head_;

            if (result)
                head_ = static_cast<T*>(head_->next);

            return result;
        }

    private:
        T* head_ = nullptr;
    };
} // namespace ThinkMeta::ConcurrentTasks::Collections
