//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Atomic128: a 16-byte word and the compare-and-exchange on it (cmpxchg16b on x86-64), for
// lock-free structures that pair a pointer with a tag. The compiler's own atomics are not used
// for it, because they differ in whether a 16-byte atomic is lock-free; this is the instruction.
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include "Defs.h"

#if defined(CT_COMPILER_MSVC)
#    include <intrin.h>
#endif

#include <atomic>
#include <cstdint>

export module ThinkMeta.ConcurrentTasks.Cpp.Atomic128;

namespace ThinkMeta::ConcurrentTasks::Cpp {
    /// <summary>A 16-byte word that <see cref="CompareExchange128"/> reads and writes as one unit.</summary>
    /// <remarks>
    /// Aligned to 16 bytes, as the instruction requires. The halves may be read on their own
    /// (<see cref="LoadLow"/>, <see cref="LoadHigh"/>) for a snapshot that a later
    /// CompareExchange128 verifies, since it fails on a torn one.
    /// </remarks>
    export struct alignas(16) Atomic128 {
        std::uint64_t low;
        std::uint64_t high;
    };

    /// <summary>Replaces <paramref name="destination"/> with <paramref name="desired"/> if it still holds <paramref name="expected"/>.</summary>
    /// <param name="destination">The word; it must be 16-byte aligned.</param>
    /// <param name="expected">The value the word is expected to hold; on failure it receives the value found.</param>
    /// <param name="desired">The new value.</param>
    /// <returns><c>true</c> if the word was replaced; <c>false</c> if it held something else.</returns>
    /// <remarks>Sequentially consistent (a full barrier), as the instruction is.</remarks>
    export inline bool CompareExchange128(Atomic128& destination, Atomic128& expected, const Atomic128& desired) noexcept
    {
#if defined(CT_COMPILER_MSVC)
        return _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(&destination), static_cast<long long>(desired.high),
                                              static_cast<long long>(desired.low), reinterpret_cast<long long*>(&expected))
            != 0;
#else
        // without -mcx16 clang would call libatomic, which may take a lock
        static_assert(__atomic_always_lock_free(16, nullptr), "build with -mcx16: the 16-byte exchange must be lock-free");
        // the builtin takes the new value through a non-const pointer, though it only reads it
        auto value = desired;
        return __atomic_compare_exchange(reinterpret_cast<unsigned __int128*>(&destination), reinterpret_cast<unsigned __int128*>(&expected),
                                         reinterpret_cast<unsigned __int128*>(&value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
    }

    // The halves are read as 8-byte atomics of a 16-byte word the exchange writes as a whole:
    // mixed-size atomics, outside the C++ memory model but coherent on x86-64 (as in
    // BoundedPointerQueue). The const_cast only lets atomic_ref name the member (atomic_ref<const T>
    // is C++26); the word itself is never const.

    /// <summary>Reads the low half on its own (acquire).</summary>
    /// <remarks>Acquire: what was written before the exchange that stored the value is visible after it.</remarks>
    export inline std::uint64_t LoadLow(const Atomic128& word) noexcept
    {
        return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(word.low)).load(std::memory_order_acquire);
    }

    /// <summary>Reads the high half on its own (acquire).</summary>
    /// <remarks>Acquire: what was written before the exchange that stored the value is visible after it.</remarks>
    export inline std::uint64_t LoadHigh(const Atomic128& word) noexcept
    {
        return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(word.high)).load(std::memory_order_acquire);
    }
} // namespace ThinkMeta::ConcurrentTasks::Cpp
