//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// VirtualMemory: owner of one reserved region of address space, plus static wrappers to commit
// and decommit pages in it. The counterpart of Windows\VirtualMemory.ixx with the same names for
// what both have; code that runs on both uses Platform.VirtualMemory.
//
// Reserving maps the region inaccessible and without backing (PROT_NONE, MAP_NORESERVE);
// committing makes pages readable and writable, which the kernel backs with zeroed pages on the
// first touch; decommitting gives the pages back (MADV_DONTNEED) and makes them inaccessible again.
//
// The file is not called VirtualMemory.ixx: file names stay unique across the library, since
// Visual Studio puts the objects of a project into one folder (MSB8027).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Posix.VirtualMemory;

export import ThinkMeta.ConcurrentTasks.Posix.Error;

namespace ThinkMeta::ConcurrentTasks::Posix {
    /// <summary>Owns one region of reserved address space (<c>mmap</c>).</summary>
    /// <remarks>
    /// The region is unmapped on destruction.
    /// Only the owner calls <see cref="Reserve"/> and <see cref="Release"/>, from one thread at a
    /// time; the static members act on any address and are as thread-safe as the system calls they
    /// wrap. Locking, protection and queries, which Windows::VirtualMemory also has, are not here yet.
    /// A commit or decommit of part of a region splits its mapping, and Linux limits the mappings of
    /// a process (<c>vm.max_map_count</c>, 65530 by default): commit contiguous windows, as the
    /// library's users do, rather than scattered pages.
    /// </remarks>
    /// <seealso cref="Memory::SpscVirtualMemoryPipe"/>
    export class VirtualMemory
    {
        VirtualMemory(const VirtualMemory&)            = delete;
        VirtualMemory(VirtualMemory&&)                 = delete;
        VirtualMemory& operator=(const VirtualMemory&) = delete;
        VirtualMemory& operator=(VirtualMemory&&)      = delete;

    public:
        /// <summary>Constructs an object that owns no memory.</summary>
        VirtualMemory() noexcept = default;

        /// <summary>Releases the region, if any.</summary>
        ~VirtualMemory() noexcept { Release(); }

        /// <summary>Returns whether the object owns a region.</summary>
        /// <returns><c>true</c> if a region is owned; otherwise <c>false</c>.</returns>
        bool IsValid() const noexcept { return memory_ != nullptr; }

        /// <summary>Returns the start of the region, or <c>nullptr</c>.</summary>
        /// <returns>The region's base address; <c>nullptr</c> if the object owns no region.</returns>
        void* GetAllocationBase() noexcept { return memory_; }

        /// <summary>Returns the address <paramref name="offset"/> bytes into the region.</summary>
        /// <param name="offset">Distance in bytes from the start of the region; must lie inside it.</param>
        /// <returns>The region's base address plus <paramref name="offset"/>.</returns>
        /// <remarks>No bounds check.</remarks>
        void* AtOffset(std::size_t offset) noexcept { return static_cast<std::byte*>(memory_) + offset; }

        /// <summary>Reserves address space.</summary>
        /// <param name="size">Bytes to reserve; rounded up to whole pages.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the <c>errno</c> of <c>mmap</c>.</returns>
        /// <remarks>
        /// Releases the current region, if any, first. Pages must be committed (<see cref="Commit"/>)
        /// before they are touched.
        /// </remarks>
        /// <seealso cref="Release"/>
        ErrorCode Reserve(std::size_t size) noexcept
        {
            Release();
            const std::size_t bytes   = RoundUp(size, PageSize());
            void*             mapping = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (mapping == MAP_FAILED)
                return Error::GetLastError();

            memory_ = mapping;
            bytes_  = bytes;
            return Error::Success;
        }

        /// <summary>Releases the whole region (<c>munmap</c>).</summary>
        /// <returns>
        /// <see cref="Error::Success"/> (<c>0</c>) on success, also if there was nothing to release;
        /// otherwise the <c>errno</c> of <c>munmap</c>.
        /// </returns>
        /// <remarks>The object owns nothing afterwards, even if <c>munmap</c> failed.</remarks>
        /// <seealso cref="Reserve"/>
        ErrorCode Release() noexcept
        {
            if (memory_) {
                const bool released = munmap(memory_, bytes_) == 0;
                memory_             = nullptr;
                bytes_              = 0;
                return Error::GetError(released);
            }

            return Error::Success;
        }

        /// <summary>Commits pages for reading and writing.</summary>
        /// <param name="address">Start of the range; rounded down to a page boundary.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is committed.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the <c>errno</c> of <c>mprotect</c>.</returns>
        /// <remarks>
        /// The range must lie inside a reservation. Pages read as zero until written, also after a
        /// <see cref="Decommit"/>.
        /// </remarks>
        /// <seealso cref="Decommit"/>
        static ErrorCode Commit(void* address, std::size_t size) noexcept
        {
            CT_ASSERT(size != 0); // an unaligned address would commit a whole page
            const Range range = PagesOf(address, size);
            return Error::GetError(mprotect(range.start, range.bytes, PROT_READ | PROT_WRITE) == 0);
        }

        /// <summary>Decommits pages.</summary>
        /// <param name="address">Start of the range; rounded down to a page boundary.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is decommitted.</param>
        /// <returns>
        /// <see cref="Error::Success"/> (<c>0</c>) on success; otherwise the <c>errno</c> of
        /// <c>madvise</c> or <c>mprotect</c>.
        /// </returns>
        /// <remarks>The address range stays reserved; its contents are gone.</remarks>
        /// <seealso cref="Commit"/>
        static ErrorCode Decommit(void* address, std::size_t size) noexcept
        {
            CT_ASSERT(size != 0); // an unaligned address would discard a whole page
            const Range range = PagesOf(address, size);
            if (madvise(range.start, range.bytes, MADV_DONTNEED) != 0)
                return Error::GetLastError();
            return Error::GetError(mprotect(range.start, range.bytes, PROT_NONE) == 0);
        }

    private:
        struct Range {
            void*       start;
            std::size_t bytes;
        };

        static std::size_t PageSize() noexcept { return static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }

        static std::size_t RoundUp(std::size_t value, std::size_t multiple) noexcept { return (value + multiple - 1) / multiple * multiple; }

        // the whole pages a range touches, as Windows rounds the range of VirtualAlloc
        static Range PagesOf(void* address, std::size_t size) noexcept
        {
            const std::size_t    page  = PageSize();
            const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(address) / page * page;
            const std::uintptr_t end   = RoundUp(reinterpret_cast<std::uintptr_t>(address) + size, page);
            return {reinterpret_cast<void*>(first), end - first};
        }

        void*       memory_{nullptr};
        std::size_t bytes_{0};
    };
} // namespace ThinkMeta::ConcurrentTasks::Posix
