//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Interfaces of sequential memory: a byte stream that a producer fills block by block and a
// consumer drains block by block, without copying. AsyncIo::AsyncFile::ReadFile writes a file
// into an IWriteSequentialMemory, AsyncFile::WriteFile reads one from an IReadSequentialMemory.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <cstddef>

export module ThinkMeta.ConcurrentTasks.Memory.SequentialMemory;

namespace ThinkMeta::ConcurrentTasks::Memory {
    /// <summary>The producer side of a sequential memory.</summary>
    /// <remarks>
    /// The stream is written front to back in blocks.
    /// Protocol: <see cref="Initialize"/> once with the expected total size, then for each block
    /// <see cref="RequestWriteMemory"/>, fill it, <see cref="ReleaseWriteMemory"/>; finally
    /// <see cref="SetEndOfStream"/> (also after an error, possibly without Initialize). Several
    /// blocks may be requested before the first is released; blocks are released in the order they
    /// were requested. A single producer: the members are not meant to be called concurrently. An
    /// implementation may suspend the calling task (e.g. to wait for room), so call them from a task.
    /// </remarks>
    /// <seealso cref="IReadSequentialMemory"/>
    /// <seealso cref="SpscVirtualMemoryPipe"/>
    export struct IWriteSequentialMemory {
        /// <summary>Announces the stream's total size in bytes.</summary>
        /// <param name="initialSize">Total number of bytes the producer will write; <c>0</c> for an empty stream.</param>
        /// <remarks>A hint for reserving memory.</remarks>
        virtual void Initialize(std::size_t initialSize) noexcept = 0;

        /// <summary>Returns the next writable block.</summary>
        /// <param name="size">Number of bytes the block must hold.</param>
        /// <returns>
        /// A block of <paramref name="size"/> bytes that directly follows the previously requested
        /// one, or <c>nullptr</c> if no memory is available (out of memory, or the end of the data
        /// when the memory has a fixed capacity).
        /// </returns>
        /// <seealso cref="ReleaseWriteMemory"/>
        virtual std::byte* RequestWriteMemory(std::size_t size) noexcept = 0;

        /// <summary>Publishes written bytes to the consumer.</summary>
        /// <param name="size">
        /// Number of bytes written into the oldest unreleased block; at most its requested size, <c>0</c> allowed.
        /// </param>
        /// <remarks>
        /// Publishes the next <paramref name="size"/> bytes of the requested blocks, i.e. the oldest
        /// requested block that is not released yet.
        /// </remarks>
        /// <seealso cref="RequestWriteMemory"/>
        virtual void ReleaseWriteMemory(std::size_t size) noexcept = 0;

        /// <summary>Marks the end of the stream.</summary>
        /// <remarks>No more data follows; wakes a waiting consumer.</remarks>
        virtual void SetEndOfStream() noexcept = 0;
    };

    /// <summary>The consumer side of a sequential memory.</summary>
    /// <remarks>
    /// The stream is read front to back in blocks.
    /// Protocol: for each block <see cref="RequestReadMemory"/>, use the bytes,
    /// <see cref="ReleaseReadMemory"/>. Several blocks may be requested before the first is
    /// released; a block stays valid until it is released. A single consumer. An implementation may
    /// suspend the calling task to wait for data, so call the members from a task.
    /// </remarks>
    /// <seealso cref="IWriteSequentialMemory"/>
    /// <seealso cref="SpscVirtualMemoryPipe"/>
    export struct IReadSequentialMemory {
        /// <summary>Returns the stream's total size in bytes.</summary>
        /// <returns>The total size announced by the producer, in bytes.</returns>
        /// <remarks>
        /// May wait until the producer has announced it (<see cref="IWriteSequentialMemory::Initialize"/>).
        /// </remarks>
        virtual std::size_t GetSize() const noexcept = 0;

        /// <summary>Returns the next block of the stream.</summary>
        /// <param name="size">
        /// On entry the wanted number of bytes; on return the number actually handed out, which is
        /// smaller only at the end of the stream.
        /// </param>
        /// <returns>The block, or <c>nullptr</c> with size <c>0</c> once the stream is exhausted.</returns>
        /// <seealso cref="ReleaseReadMemory"/>
        virtual const std::byte* RequestReadMemory(std::size_t& size) noexcept = 0;

        /// <summary>Gives back consumed bytes.</summary>
        /// <param name="size">
        /// The oldest bytes handed out by <see cref="RequestReadMemory"/>; <c>0</c> is allowed.
        /// </param>
        /// <remarks>The memory may then be reused or freed.</remarks>
        virtual void ReleaseReadMemory(std::size_t size) noexcept = 0;
    };
} // namespace ThinkMeta::ConcurrentTasks::Memory