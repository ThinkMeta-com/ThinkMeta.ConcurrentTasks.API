//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// SpscVirtualMemoryPipe: a single-producer single-consumer sequential memory over one reserved
// range of virtual memory. Pages are committed as the producer writes and decommitted as the
// consumer releases, so a large stream passes through while both sides run.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <atomic>
#include <cstddef>

export module ThinkMeta.ConcurrentTasks.Memory.SpscVirtualMemoryPipe;
export import ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;

import ThinkMeta.ConcurrentTasks.Windows.VirtualMemory;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;

namespace ThinkMeta::ConcurrentTasks::Memory {
    /// <summary>A zero-copy pipe between one producer task and one consumer task.</summary>
    /// <remarks>
    /// The producer writes through <see cref="IWriteSequentialMemory"/>, the consumer reads through
    /// <see cref="IReadSequentialMemory"/>; both get pointers into a contiguous virtual address
    /// range, so neither side copies.
    /// <see cref="Initialize"/> reserves address space for the whole announced size (rounded up to
    /// 64 KiB); the producer must not write more than that. Memory is committed in 64 KiB steps on
    /// <see cref="RequestWriteMemory"/> and decommitted in 64 KiB steps once the consumer has
    /// released them, so the committed memory is about the distance between the two sides.
    /// The consumer waits (suspends) in <see cref="GetSize"/> until Initialize and in
    /// <see cref="RequestReadMemory"/> until enough data or the end of the stream has arrived; the
    /// producer never waits. Both sides must therefore be tasks (or converted threads). A failure to
    /// reserve, commit or decommit memory ends the process. The producer calls Initialize also for
    /// an empty stream, since GetSize waits for it.
    /// </remarks>
    /// <seealso cref="Windows::VirtualMemory"/>
    export class SpscVirtualMemoryPipe : public IReadSequentialMemory, public IWriteSequentialMemory
    {
        SpscVirtualMemoryPipe(const SpscVirtualMemoryPipe&)            = delete;
        SpscVirtualMemoryPipe(SpscVirtualMemoryPipe&&)                 = delete;
        SpscVirtualMemoryPipe& operator=(const SpscVirtualMemoryPipe&) = delete;
        SpscVirtualMemoryPipe& operator=(SpscVirtualMemoryPipe&&)      = delete;

    public:
        /// <summary>Constructs an empty pipe.</summary>
        /// <remarks>The producer calls <see cref="Initialize"/> first.</remarks>
        SpscVirtualMemoryPipe() noexcept = default;

        // Inherited via IReadSequentialMemory

        /// <summary>Returns the size announced by <see cref="Initialize"/>.</summary>
        /// <returns>The announced stream size in bytes.</returns>
        /// <remarks>Suspends the caller until Initialize has been called.</remarks>
        std::size_t GetSize() const noexcept
        {
            sizeAvailableEvent_.Wait();
            return memorySize_;
        }

        /// <summary>Returns the next block of the stream.</summary>
        /// <param name="size">
        /// On entry the wanted number of bytes; at the end of the stream it shrinks to what is left.
        /// </param>
        /// <returns>The block, or <c>nullptr</c> with size <c>0</c> once the stream is exhausted.</returns>
        /// <remarks>
        /// Suspends until the producer has released that many bytes
        /// (<see cref="ReleaseWriteMemory"/>) or has set the end of the stream
        /// (<see cref="SetEndOfStream"/>).
        /// </remarks>
        /// <seealso cref="ReleaseReadMemory"/>
        const std::byte* RequestReadMemory(std::size_t& size) noexcept override
        {
            // written_ and endOfStream_ are written by the producer; readMemoryAvailableEvent_ is
            // set after each change, so a consumer that saw too little waits for the next change
            // and checks again. An auto-reset event suffices with a single consumer.
            // The end of the stream is read first: the producer publishes its last bytes before it
            // sets the end, so a consumer that sees the end (acquire) sees every byte as well. The
            // other way round the bytes of the last block could be missed and the stream cut short.
            for (;;) {
                const bool endOfStream = endOfStream_.load(std::memory_order_acquire);
                const auto available   = written_.load(std::memory_order_acquire) - readPosition_;
                if (available >= size)
                    break;

                if (endOfStream) {
                    if (available > 0) {
                        size = available;
                        break;
                    }
                    size = 0;
                    return nullptr;
                }

                readMemoryAvailableEvent_.Wait();
            }

            auto result = virtualMemory_.AtOffset(readPosition_);
            readPosition_ += size;
            return static_cast<const std::byte*>(result);
        }

        /// <summary>Releases consumed bytes.</summary>
        /// <param name="size">
        /// Number of the oldest bytes handed out by <see cref="RequestReadMemory"/> that are no longer used;
        /// <c>0</c> is allowed.
        /// </param>
        /// <remarks>Whole 64 KiB steps behind the consumer are decommitted.</remarks>
        void ReleaseReadMemory(std::size_t size) noexcept override
        {
            // read_ counts released bytes above minCommitPosition_; only whole commit steps are
            // decommitted, the remainder waits for the next release.
            read_ += size;
            if (read_ >= CommitSize) {
                auto sizeToDecommit = (read_ / CommitSize) * CommitSize;
                Windows::ErrorException::ThrowIfFailed(Windows::VirtualMemory::Decommit(virtualMemory_.AtOffset(minCommitPosition_), sizeToDecommit));
                minCommitPosition_ += sizeToDecommit;
                read_ -= sizeToDecommit;
            }
        }

        // Inherited via IWriteSequentialMemory

        /// <summary>Reserves address space for the stream.</summary>
        /// <param name="initialSize">
        /// Total number of bytes the producer will write; <c>0</c> for an empty stream (nothing is reserved).
        /// </param>
        /// <remarks>Call once. Releases a consumer waiting in <see cref="GetSize"/>.</remarks>
        void Initialize(std::size_t initialSize) noexcept override
        {
            // an empty stream reserves nothing (VirtualAlloc rejects a size of 0)
            auto reservedSize = ((initialSize + CommitSize - 1) / CommitSize) * CommitSize;
            if (reservedSize)
                Windows::ErrorException::ThrowIfFailed(virtualMemory_.Reserve(reservedSize));
            memorySize_ = initialSize;
            sizeAvailableEvent_.Set();
        }

        /// <summary>Returns the next writable block, committing pages as needed.</summary>
        /// <param name="size">
        /// Number of bytes the block must hold; the total must stay within the size given to <see cref="Initialize"/>.
        /// </param>
        /// <returns>The block; never <c>nullptr</c>.</returns>
        /// <remarks>
        /// The producer stays within the size announced by <see cref="Initialize"/>; a failed commit
        /// ends the process.
        /// </remarks>
        /// <seealso cref="ReleaseWriteMemory"/>
        std::byte* RequestWriteMemory(std::size_t size) noexcept override
        {
            if (writePosition_ + size > maxCommitPosition_) {
                auto sizeToCommit = ((writePosition_ + size - maxCommitPosition_ + CommitSize - 1) / CommitSize) * CommitSize;
                Windows::ErrorException::ThrowIfFailed(
                    Windows::VirtualMemory::Commit(virtualMemory_.AtOffset(maxCommitPosition_), sizeToCommit, PAGE_READWRITE));
                maxCommitPosition_ += sizeToCommit;
            }

            auto result = virtualMemory_.AtOffset(writePosition_);
            writePosition_ += size;
            return static_cast<std::byte*>(result);
        }

        /// <summary>Publishes written bytes to the consumer and wakes it.</summary>
        /// <param name="size">
        /// Number of bytes written into the oldest unreleased block; at most its requested size, <c>0</c> allowed.
        /// </param>
        /// <seealso cref="RequestReadMemory"/>
        void ReleaseWriteMemory(std::size_t size) noexcept override
        {
            // release: the bytes are visible to a consumer that sees the new count
            written_.fetch_add(size, std::memory_order_release);
            readMemoryAvailableEvent_.Set();
        }

        /// <summary>Marks the end of the stream and wakes the consumer.</summary>
        void SetEndOfStream() noexcept override
        {
            // release, after the last ReleaseWriteMemory: see RequestReadMemory
            endOfStream_.store(true, std::memory_order_release);
            readMemoryAvailableEvent_.Set();
        }

    private:
        // Granularity of commit and decommit (also of the reservation): the allocation granularity.
        static constexpr std::size_t CommitSize = 65536;

        Windows::VirtualMemory                      virtualMemory_;
        mutable Synchronization::ManualResetEvent_s sizeAvailableEvent_;
        Synchronization::AutoResetEvent_s           readMemoryAvailableEvent_;

        // Positions are byte offsets from the start of the reserved range:
        // writePosition_ = end of the blocks handed to the producer, written_ = end of the released
        // ones; readPosition_ = end of the blocks handed to the consumer, read_ = released bytes not
        // yet decommitted, counted from minCommitPosition_; [minCommitPosition_, maxCommitPosition_)
        // is committed.
        // written_ and endOfStream_ cross from the producer to the consumer; the rest belongs to
        // one side each.
        std::size_t              memorySize_{0};
        std::size_t              writePosition_{0};
        std::size_t              readPosition_{0};
        std::atomic<std::size_t> written_{0};
        std::size_t              read_{0};
        std::size_t              minCommitPosition_{0};
        std::size_t              maxCommitPosition_{0};
        std::atomic<bool>        endOfStream_{false};
    };
} // namespace ThinkMeta::ConcurrentTasks::Memory
