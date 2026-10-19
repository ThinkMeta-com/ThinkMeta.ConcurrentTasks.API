//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// BoundedPointerQueue: a bounded lock-free multi-producer multi-consumer FIFO of pointers. The
// Default scheduler's task queues are built on it; it is also compiled into the CRT-free core.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <atomic>
#include <cstdint>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Collections.BoundedPointerQueue;

import ThinkMeta.ConcurrentTasks.Cpp.Padded;

namespace ThinkMeta::ConcurrentTasks::Collections {
    /// <summary>A bounded, lock-free, multi-producer multi-consumer queue for pointers.</summary>
    /// <typeparam name="T">The pointee type; the queue holds <typeparamref name="T"/>* and does not own them.</typeparam>
    /// <remarks>
    /// Meant for cases where the number of elements is known and bounded, such as the scheduler's
    /// task queues. The capacity is set once, at run time, by <see cref="Initialize"/> and is a
    /// power of two. It must cover the maximum number of elements the use case can hold at once:
    /// enqueueing into a full queue is invalid use and leaves the queue unusable.
    /// </remarks>
    /// <seealso cref="BoundedPointerQueue"/>
    template <typename T>
    class _BoundedPointerQueue
    {
    public:
        /// <summary>Constructs a queue without storage.</summary>
        /// <remarks><see cref="Initialize"/> must succeed before the queue is used.</remarks>
        _BoundedPointerQueue() noexcept
        {
            tail_.store(0, std::memory_order_relaxed);
            head_.store(0, std::memory_order_relaxed);
        }

        /// <summary>Frees the ring.</summary>
        /// <remarks>Pointers still in the queue are not touched (not owned).</remarks>
        ~_BoundedPointerQueue() noexcept
        {
            if (ring_.nodes)
                VirtualFree(ring_.nodes, 0, MEM_RELEASE);
        }

        _BoundedPointerQueue(const _BoundedPointerQueue&)            = delete;
        _BoundedPointerQueue(_BoundedPointerQueue&&)                 = delete;
        _BoundedPointerQueue& operator=(const _BoundedPointerQueue&) = delete;
        _BoundedPointerQueue& operator=(_BoundedPointerQueue&&)      = delete;

        /// <summary>Allocates the ring.</summary>
        /// <param name="capacity">Number of slots; must be a power of two and at least 2.</param>
        /// <returns><c>true</c> on success; <c>false</c> if the memory could not be allocated.</returns>
        /// <remarks>Call once, before the queue is shared.</remarks>
        /// <seealso cref="Capacity"/>
        bool Initialize(std::size_t capacity) noexcept
        {
            CT_ASSERT(!ring_.nodes);
            CT_ASSERT(capacity >= 2 && (capacity & (capacity - 1)) == 0);

            // VirtualAlloc hands out zeroed, page-aligned memory, so every node starts empty (sequence 0)
            // on its own cache line, and pages nobody touches cost no physical memory.
            auto* nodes = static_cast<Node*>(VirtualAlloc(nullptr, capacity * sizeof(Node), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!nodes)
                return false;

            ring_.nodes    = nodes;
            ring_.capacity = capacity;
            ring_.mask     = capacity - 1;
            // A loop rather than std::countr_zero, which may need the CRT's __isa_available; this
            // module is also built into the core, which has no CRT.
            while ((std::size_t{1} << ring_.shift) < capacity)
                ++ring_.shift;
            return true;
        }

        /// <summary>The number of slots, <c>0</c> before <see cref="Initialize"/>.</summary>
        /// <returns>The capacity passed to <see cref="Initialize"/>, or <c>0</c>.</returns>
        std::size_t Capacity() const noexcept { return ring_.capacity; }

        /// <summary>Enqueues a pointer.</summary>
        /// <param name="ptr">Pointer to enqueue. Must not be <c>nullptr</c>.</param>
        /// <remarks>
        /// Lock-free; callable from any number of threads at once. The queue must not be full (see
        /// the capacity rule above).
        /// </remarks>
        /// <seealso cref="Dequeue"/>
        // On a full queue the loop advances tail_ past occupied slots and keeps spinning; if it
        // gets a slot later, head_ and tail_ no longer match the slots.
        void Enqueue(T* ptr) noexcept
        {
            // Slot protocol: in round r (= index >> shift) a slot is free for enqueue at sequence
            // 2r and holds a value for dequeue at sequence 2r + 1; dequeue moves it to 2(r + 1), the
            // free state of the next round. value and sequence are swapped together in one 16-byte
            // CAS, so a stale producer cannot write into a slot of another round (ABA).
            for (;;) {
                // Load the current tail index and calculate the expected sequence number for the slot.
                auto slotIndex        = tail_.load(std::memory_order_relaxed);
                auto expectedSequence = (slotIndex >> ring_.shift) * 2;

                std::atomic_ref<_Node> slotRef(ring_.nodes[slotIndex & ring_.mask]);
                _Node                  expected = slotRef.load(std::memory_order_acquire);

                // The slot is free for this round only at the expected sequence (the value is not
                // checked; it may be a stale one). Otherwise:
                // 1. 2r + 1: another thread has already enqueued an item in this slot.
                // 2. 2r + 2: the item has already been dequeued again (tail_ was read stale).
                // 3. 2r - 1: the item of the previous round is still there (queue full).
                if (expected.sequence == expectedSequence) {
                    _Node newValue{ptr, expected.sequence + 1};

                    if (slotRef.compare_exchange_strong(expected, newValue, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        // Either the current thread will advance the tail index, or another thread will do it (see below).
                        tail_.compare_exchange_weak(slotIndex, slotIndex + 1, std::memory_order_relaxed, std::memory_order_relaxed);
                        return;
                    }
                }

                // Help advance the tail index if another thread has already enqueued an item.
                tail_.compare_exchange_weak(slotIndex, slotIndex + 1, std::memory_order_relaxed, std::memory_order_relaxed);
            }
        }

        /// <summary>Dequeues a pointer.</summary>
        /// <returns>The dequeued pointer, or <c>nullptr</c> if the queue is empty.</returns>
        /// <remarks>
        /// Lock-free; callable from any number of threads at once. Under contention the queue can
        /// look empty for a moment while another consumer is taking an element, so <c>nullptr</c>
        /// means "try again later", not proof of emptiness.
        /// </remarks>
        /// <seealso cref="Enqueue"/>
        // That moment: another consumer has taken the head slot but not yet advanced head_. Only
        // that consumer advances head_ (a consumer that finds the slot taken returns nullptr
        // without helping), so this relies on its compare_exchange_weak not failing spuriously:
        // true on x64 (lock cmpxchg); on LL/SC platforms head_ would stay behind for good.
        T* Dequeue() noexcept
        {
            for (;;) {
                // Load the current head index.
                auto slotIndex        = head_.load(std::memory_order_relaxed);
                auto expectedSequence = (slotIndex >> ring_.shift) * 2 + 1;

                // Load the node at the calculated slot index.
                auto&                  slot = ring_.nodes[slotIndex & ring_.mask];
                std::atomic_ref<_Node> slotRef(slot);
                _Node                  node = slotRef.load(std::memory_order_acquire);

                // The slot holds an item of this round only at sequence 2r + 1; anything else means
                // empty or just taken by another consumer.
                if (node.sequence != expectedSequence)
                    return nullptr;

                // Advance only the sequence, to 2(r + 1), the free state of the next round. The stale
                // value stays in the slot and is ignored, because the sequence alone decides whether
                // a slot is full.
                // This 8-byte CAS on slot.sequence and the 16-byte accesses to the whole slot in
                // Enqueue touch the same memory with different widths. The C++ memory model does not
                // cover such mixed-size atomics; x64 keeps them coherent (a second x64 assumption next
                // to the compare_exchange_weak on head_, see above).
                auto                           newSequence{node.sequence + 1};
                std::atomic_ref<std::uint64_t> sequenceRef(slot.sequence);

                // Attempt to advance the sequence.
                // There is one reason why this might fail:
                // 1. Another thread has already dequeued the item in this slot.
                if (sequenceRef.compare_exchange_strong(node.sequence, newSequence, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    head_.compare_exchange_weak(slotIndex, slotIndex + 1, std::memory_order_relaxed, std::memory_order_relaxed);
                    return node.value;
                }

                head_.compare_exchange_weak(slotIndex, slotIndex + 1, std::memory_order_relaxed, std::memory_order_relaxed);
            }
        }

    private:
        /// <summary>One slot of the queue.</summary>
        /// <remarks>Contains the pointer value and a sequence number for ABA prevention.</remarks>
        struct _Node {
            /// <summary>Pointer value stored in the node.</summary>
            T* value;

            /// <summary>Sequence number for ABA prevention.</summary>
            std::uint64_t sequence;
        };

        /// <summary>Node type padded to cache line size to avoid false sharing.</summary>
        using Node = Cpp::Padded<_Node>;

        static_assert(sizeof(Node) == CT_CACHE_LINE_SIZE, "Node size must be aligned to avoid false sharing");
        static_assert(std::atomic_ref<_Node>::is_always_lock_free);

        /// <summary>Where the ring lives and how an index maps onto it.</summary>
        /// <remarks>
        /// Written once by <see cref="Initialize"/> and only read afterwards, so it shares its cache
        /// line with nothing that is written.
        /// </remarks>
        struct _Ring {
            /// <summary>The circular buffer of nodes.</summary>
            Node* nodes = nullptr;

            /// <summary>Number of nodes, a power of two.</summary>
            std::size_t capacity = 0;

            /// <summary>Mask for wrapping indices (capacity - 1).</summary>
            std::size_t mask = 0;

            /// <summary>log2(capacity): index >> shift is the round an index belongs to.</summary>
            std::uint32_t shift = 0;
        };

        Cpp::Padded<_Ring> ring_;

        /// <summary>Head index for dequeue operations.</summary>
        Cpp::Padded<std::atomic<std::size_t>> head_;

        /// <summary>Tail index for enqueue operations.</summary>
        Cpp::Padded<std::atomic<std::size_t>> tail_;
    };

    /// <summary>A bounded, lock-free, multi-producer multi-consumer FIFO of pointers.</summary>
    /// <typeparam name="T">The pointee type; the queue holds <typeparamref name="T"/>* and does not own them.</typeparam>
    /// <remarks>
    /// Holds <typeparamref name="T"/>* pointers and occupies whole cache lines of its own
    /// (<see cref="Cpp::Padded"/>). Construct, then <see cref="_BoundedPointerQueue::Initialize"/>
    /// with a power-of-two capacity.
    /// </remarks>
    /// <seealso cref="_BoundedPointerQueue"/>
    export template <typename T>
    using BoundedPointerQueue = Cpp::Padded<_BoundedPointerQueue<T>>;
} // namespace ThinkMeta::ConcurrentTasks::Collections
