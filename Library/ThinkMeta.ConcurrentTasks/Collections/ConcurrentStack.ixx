//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// ConcurrentStack: a lock-free intrusive LIFO list of ListEntry-derived items. Two
// implementations with the same interface: the library's own (Detail::LockFreeStack, a head of
// pointer and tag changed with a 16-byte compare-and-exchange), the default on every platform,
// and, on Windows, the interlocked SList of the system (Detail::SListStack), whose entry has the
// layout of ListEntry, for comparison. Which one ConcurrentStack names is decided at the end of
// this file.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_PLATFORM_WINDOWS)
#    include <Windows.h>
#endif

#include <atomic>
#include <cstdint>
#include <type_traits>

#include <ThinkMeta.ConcurrentTasks/Core.h>

export module ThinkMeta.ConcurrentTasks.Collections.ConcurrentStack;

import ThinkMeta.ConcurrentTasks.Cpp.Atomic128;

namespace ThinkMeta::ConcurrentTasks::Collections::Detail {
    /// <summary>The core's own lock-free stack: a head of top pointer and tag, changed as one 16-byte word.</summary>
    /// <remarks>
    /// The tag counts every change in its upper 48 bits and the depth in its lower 16, so a head
    /// that was popped and pushed back (ABA) never passes for the one a thread read. Pop reads the
    /// top item's <c>next</c> before its exchange may fail, so an item must stay readable until no
    /// Pop that could have seen it on the stack is in flight any more; items of the core (task
    /// contexts, per-thread data) live as long as their scheduler, which settles it. Unlike the
    /// Windows SList, whose pop the kernel restarts when it faults on a freed item, a read of freed
    /// memory here is a crash.
    ///
    /// <c>next</c> is written by a pusher and read by a concurrent popper that may lose its race,
    /// so both go through an atomic reference (relaxed: a plain move on x86-64); the head is read
    /// with acquire and changed with a full barrier, which orders <c>next</c> around it.
    /// </remarks>
    export template <typename T>
    requires std::is_base_of_v<ListEntry, T>
    class LockFreeStack
    {
    public:
        /// <summary>Constructs an empty stack.</summary>
        /// <remarks>Constant initialization: a global stack is ready before any code runs.</remarks>
        constexpr LockFreeStack() noexcept : head_{0, 0} {}

        LockFreeStack(const LockFreeStack&)            = delete;
        LockFreeStack(LockFreeStack&&)                 = delete;
        LockFreeStack& operator=(const LockFreeStack&) = delete;
        LockFreeStack& operator=(LockFreeStack&&)      = delete;

        /// <summary>Pushes an item on top.</summary>
        /// <param name="item">Not <c>nullptr</c>; its <see cref="ListEntry::next"/> is overwritten.</param>
        void Push(T* item) noexcept
        {
            Cpp::Atomic128 expected = Snapshot();
            Cpp::Atomic128 desired;
            do {
                NextOf(item).store(Top(expected), std::memory_order_relaxed);
                desired.low  = reinterpret_cast<std::uint64_t>(static_cast<ListEntry*>(item));
                desired.high = NextTag(expected.high, 1);
            } while (!Cpp::CompareExchange128(head_, expected, desired));
        }

        /// <summary>Removes and returns the top item.</summary>
        /// <returns>The top item, or <c>nullptr</c> if the stack is empty.</returns>
        /// <remarks>
        /// Reads the top item's <c>next</c> before its exchange can fail: an item that leaves the
        /// stack must stay readable until every <c>Pop</c> that may have seen it on top has returned
        /// (see the class remarks). Lifetime is the user's matter; a freed item here is a crash.
        /// </remarks>
        T* Pop() noexcept
        {
            Cpp::Atomic128 expected = Snapshot();
            Cpp::Atomic128 desired;
            do {
                auto top = Top(expected);
                if (!top)
                    return nullptr;

                desired.low  = reinterpret_cast<std::uint64_t>(NextOf(top).load(std::memory_order_relaxed));
                desired.high = NextTag(expected.high, -1);
            } while (!Cpp::CompareExchange128(head_, expected, desired));

            return static_cast<T*>(Top(expected));
        }

        /// <summary>Removes all items in one operation and returns them as a chain.</summary>
        /// <returns>
        /// The former top item, linked to the others through <see cref="ListEntry::next"/>; <c>nullptr</c>
        /// if the stack was empty.
        /// </returns>
        /// <remarks>
        /// A <c>Pop</c> that lost its race may still read the <c>next</c> of an item in the chain:
        /// walk it with relaxed atomic loads (<c>std::atomic_ref</c>), as the stack itself does.
        /// </remarks>
        T* Flush() noexcept
        {
            Cpp::Atomic128 expected = Snapshot();
            Cpp::Atomic128 desired;
            do {
                if (!Top(expected))
                    return nullptr;

                desired.low  = 0;
                desired.high = (expected.high >> DepthBits << DepthBits) + (std::uint64_t{1} << DepthBits); // depth 0, next sequence
            } while (!Cpp::CompareExchange128(head_, expected, desired));

            return static_cast<T*>(Top(expected));
        }

        /// <summary>Returns the number of items at the moment of the call.</summary>
        /// <returns>The item count, modulo 65536.</returns>
        std::uint16_t Size() const noexcept { return static_cast<std::uint16_t>(Cpp::LoadHigh(head_) & DepthMask); }

    private:
        static constexpr unsigned      DepthBits = 16;
        static constexpr std::uint64_t DepthMask = (std::uint64_t{1} << DepthBits) - 1;

        // The two halves read on their own; a torn pair fails the exchange, which then hands back
        // the real one.
        Cpp::Atomic128 Snapshot() const noexcept { return {Cpp::LoadLow(head_), Cpp::LoadHigh(head_)}; }

        static ListEntry* Top(const Cpp::Atomic128& head) noexcept { return reinterpret_cast<ListEntry*>(head.low); }

        static std::atomic_ref<ListEntry*> NextOf(ListEntry* entry) noexcept { return std::atomic_ref<ListEntry*>(entry->next); }

        // the next sequence number, and the depth changed by delta (modulo 2^16, as the SList counts)
        static std::uint64_t NextTag(std::uint64_t tag, std::int64_t delta) noexcept
        {
            const std::uint64_t sequence = (tag >> DepthBits) + 1;
            const std::uint64_t depth    = (tag + static_cast<std::uint64_t>(delta)) & DepthMask;
            return sequence << DepthBits | depth;
        }

        Cpp::Atomic128 head_;
    };

#if defined(CT_PLATFORM_WINDOWS)
    /// <summary>The Windows interlocked SList, with ListEntry as its entry.</summary>
    export template <typename T>
    requires std::is_base_of_v<ListEntry, T>
    class SListStack
    {
        static_assert(sizeof(ListEntry) == sizeof(SLIST_ENTRY) && alignof(ListEntry) == MEMORY_ALLOCATION_ALIGNMENT);

    public:
        /// <summary>Constructs an empty stack.</summary>
        SListStack() noexcept { InitializeSListHead(&head_); }

        SListStack(const SListStack&)            = delete;
        SListStack(SListStack&&)                 = delete;
        SListStack& operator=(const SListStack&) = delete;
        SListStack& operator=(SListStack&&)      = delete;

        /// <summary>Pushes an item on top.</summary>
        /// <param name="item">Not <c>nullptr</c>; its <see cref="ListEntry::next"/> is overwritten.</param>
        void Push(T* item) noexcept { InterlockedPushEntrySList(&head_, ToSListEntry(item)); }

        /// <summary>Removes and returns the top item.</summary>
        /// <returns>The top item, or <c>nullptr</c> if the stack is empty.</returns>
        T* Pop() noexcept { return FromSListEntry(InterlockedPopEntrySList(&head_)); }

        /// <summary>Removes all items in one operation and returns them as a chain.</summary>
        /// <returns>The former top item, linked through <see cref="ListEntry::next"/>; <c>nullptr</c> if empty.</returns>
        T* Flush() noexcept { return FromSListEntry(InterlockedFlushSList(&head_)); }

        /// <summary>Returns the number of items at the moment of the call.</summary>
        /// <returns>The item count, modulo 65536.</returns>
        std::uint16_t Size() const noexcept { return QueryDepthSList(&head_); }

    private:
        static SLIST_ENTRY* ToSListEntry(T* item) noexcept { return reinterpret_cast<SLIST_ENTRY*>(static_cast<ListEntry*>(item)); }
        static T*           FromSListEntry(SLIST_ENTRY* entry) noexcept { return static_cast<T*>(reinterpret_cast<ListEntry*>(entry)); }

        // mutable because QueryDepthSList takes a non-const header even though it only reads.
        mutable SLIST_HEADER head_;
    };
#endif
} // namespace ThinkMeta::ConcurrentTasks::Collections::Detail

namespace ThinkMeta::ConcurrentTasks::Collections {
    /// <summary>A lock-free intrusive LIFO stack.</summary>
    /// <typeparam name="T">
    /// The item type; derives from <see cref="ListEntry"/>, through which items are linked.
    /// </typeparam>
    /// <remarks>
    /// <c>Push</c> and <c>Pop</c> may be called from any number of threads and tasks at once.
    /// Items are linked through their <see cref="ListEntry"/> base, so pushing never allocates.
    /// The stack does not own its items; an item must stay alive while it is on the stack, and
    /// readable while a <c>Pop</c> that may have seen it there is still running. Destroying a
    /// non-empty stack just forgets the items.
    /// <c>Flush</c> removes all items in one operation and returns them as a chain through
    /// <see cref="ListEntry::next"/>: the stack is empty afterwards and is not touched by walking
    /// the chain, so a caller that must not access the stack after a certain point (the object may
    /// go away) takes the chain first and works through it afterwards. Read an item's <c>next</c>
    /// before handing the item to anybody else. <c>Size</c> is a snapshot, modulo 65536.
    /// The stack is the library's own on every platform; built with <c>CT_CONCURRENT_STACK_SLIST</c>
    /// on Windows, it is the system's interlocked SList instead (the two were measured alike).
    /// </remarks>
    /// <seealso cref="BasicStack"/>
#if defined(CT_PLATFORM_WINDOWS) && defined(CT_CONCURRENT_STACK_SLIST)
    export template <typename T>
    using ConcurrentStack = Detail::SListStack<T>;
#else
    export template <typename T>
    using ConcurrentStack = Detail::LockFreeStack<T>;
#endif
} // namespace ThinkMeta::ConcurrentTasks::Collections
