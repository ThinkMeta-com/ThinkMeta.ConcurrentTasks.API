//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// VirtualMemory: owner of one VirtualAlloc'ed region, plus static wrappers for commit,
// decommit, lock, protect and query on any address. Errors are returned as ErrorCode.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <Windows.h>

#include <cstddef>

export module ThinkMeta.ConcurrentTasks.Windows.VirtualMemory;

export import ThinkMeta.ConcurrentTasks.Windows.Error;

namespace ThinkMeta::ConcurrentTasks::Windows {
    /// <summary>Owns one region allocated with <c>VirtualAlloc</c>.</summary>
    /// <remarks>
    /// The region is released (<c>MEM_RELEASE</c>) on destruction.
    /// Only the owner calls <see cref="Reserve"/> and <see cref="Release"/>, from one thread at a
    /// time; the static members act on any address and are as thread-safe as the Win32 calls they
    /// wrap.
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

        /// <summary>Allocates a region (<c>VirtualAlloc</c>).</summary>
        /// <param name="size">Size in bytes; rounded up to whole pages.</param>
        /// <param name="allocationType">
        /// <c>MEM_RESERVE</c>, <c>MEM_COMMIT</c> or both, optionally with <c>MEM_LARGE_PAGES</c> or
        /// <c>MEM_TOP_DOWN</c>.
        /// </param>
        /// <param name="protection">Page protection, e.g. <c>PAGE_READWRITE</c> or <c>PAGE_NOACCESS</c>.</param>
        /// <remarks>
        /// Calls <c>VirtualAlloc(nullptr, size, allocationType, protection)</c>. Check
        /// <see cref="IsValid"/>; on failure <c>GetLastError</c> has the reason.
        /// </remarks>
        VirtualMemory(std::size_t size, DWORD allocationType, DWORD protection) noexcept { memory_ = ::VirtualAlloc(nullptr, size, allocationType, protection); }

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
        void* AtOffset(std::size_t offset) noexcept { return static_cast<char*>(memory_) + offset; }

        /// <summary>Reserves address space.</summary>
        /// <param name="size">Bytes to reserve (<c>MEM_RESERVE</c>, <c>PAGE_NOACCESS</c>).</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>
        /// Releases the current region, if any, first. Pages must be committed (<see cref="Commit"/>)
        /// before they are touched.
        /// </remarks>
        /// <seealso cref="Release"/>
        ErrorCode Reserve(std::size_t size) noexcept
        {
            Release();
            memory_ = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
            return Error::GetError(IsValid());
        }

        /// <summary>Releases the whole region (<c>MEM_RELEASE</c>).</summary>
        /// <returns>
        /// <see cref="Error::Success"/> (<c>0</c>) on success, also if there was nothing to release;
        /// otherwise the Win32 error code.
        /// </returns>
        /// <remarks>The object owns nothing afterwards, even if <c>VirtualFree</c> failed.</remarks>
        /// <seealso cref="Reserve"/>
        ErrorCode Release() noexcept
        {
            if (memory_) {
                BOOL b  = VirtualFree(memory_, 0, MEM_RELEASE);
                memory_ = nullptr;
                return Error::GetError(b);
            }

            return Error::Success;
        }

        /// <summary>Commits pages (<c>MEM_COMMIT</c>).</summary>
        /// <param name="address">Start of the range; rounded down to a page boundary.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is committed.</param>
        /// <param name="protection">Page protection, e.g. <c>PAGE_READWRITE</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The range must lie inside an existing reservation.</remarks>
        /// <seealso cref="Decommit"/>
        static ErrorCode Commit(void* address, std::size_t size, DWORD protection) noexcept
        {
            return Error::GetError(VirtualAlloc(address, size, MEM_COMMIT, protection) != nullptr);
        }

        /// <summary>Decommits pages (<c>MEM_DECOMMIT</c>).</summary>
        /// <param name="address">Start of the range; rounded down to a page boundary.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is decommitted.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>The address range stays reserved.</remarks>
        /// <seealso cref="Commit"/>
        static ErrorCode Decommit(void* address, std::size_t size) noexcept { return Error::GetError(VirtualFree(address, size, MEM_DECOMMIT)); }

        /// <summary>Locks pages into physical memory (<c>VirtualLock</c>).</summary>
        /// <param name="address">Start of the range; the pages must be committed.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is locked.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Limited by the working set size.</remarks>
        /// <seealso cref="Unlock"/>
        static ErrorCode Lock(void* address, std::size_t size) noexcept { return Error::GetError(VirtualLock(address, size)); }

        /// <summary>Unlocks pages locked with <see cref="Lock"/> (<c>VirtualUnlock</c>).</summary>
        /// <param name="address">Start of the range.</param>
        /// <param name="size">Length of the range in bytes; every page it touches is unlocked.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        static ErrorCode Unlock(void* address, std::size_t size) noexcept { return Error::GetError(VirtualUnlock(address, size)); }

        /// <summary>Changes the protection of committed pages (<c>VirtualProtect</c>).</summary>
        /// <param name="address">Start of the range; the pages must be committed.</param>
        /// <param name="size">Length of the range in bytes; every page it touches changes.</param>
        /// <param name="protection">The new page protection, e.g. <c>PAGE_READONLY</c>.</param>
        /// <param name="oldProtection">Receives the old protection of the first page; required.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        static ErrorCode Protect(void* address, std::size_t size, DWORD protection, DWORD* oldProtection) noexcept
        {
            return Error::GetError(VirtualProtect(address, size, protection, oldProtection));
        }

        /// <summary>Changes the protection of committed pages (<c>VirtualProtect</c>).</summary>
        /// <param name="address">Start of the range; the pages must be committed.</param>
        /// <param name="size">Length of the range in bytes; every page it touches changes.</param>
        /// <param name="protection">The new page protection, e.g. <c>PAGE_READONLY</c>.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        /// <remarks>Discards the old protection.</remarks>
        static ErrorCode Protect(void* address, std::size_t size, DWORD protection) noexcept
        {
            DWORD oldProtection;
            return Protect(address, size, protection, &oldProtection);
        }

        /// <summary>Describes the pages around an address (<c>VirtualQuery</c>).</summary>
        /// <param name="address">Any address; the pages around it are described.</param>
        /// <param name="mbi">Receives the description; required.</param>
        /// <returns><see cref="Error::Success"/> (<c>0</c>) on success; otherwise the Win32 error code.</returns>
        static ErrorCode Query(const void* address, MEMORY_BASIC_INFORMATION* mbi) noexcept
        {
            return Error::GetError(VirtualQuery(address, mbi, sizeof(*mbi)) > 0);
        }

    private:
        void* memory_{nullptr};
    };
} // namespace ThinkMeta::ConcurrentTasks::Windows