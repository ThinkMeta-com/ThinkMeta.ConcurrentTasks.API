//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Padded: wraps a type so that each object fills whole cache lines, which keeps independently
// written data (queue indices, per-thread counters) from sharing a line (false sharing).
//
// Preliminary API: everything in this file is subject to change.
//

module;

#include <array>
#include <type_traits>

#include "Defs.h"

export module ThinkMeta.ConcurrentTasks.Cpp.Padded;

namespace ThinkMeta::ConcurrentTasks::Cpp {
    // T whose size is not a multiple of PadSize: derives from T and appends padding up to the
    // next multiple. Inherits T's constructors.
    template <typename T, std::size_t PadSize>
    struct alignas(PadSize) _Padded : T {
        using T::T;

    private:
        std::array<char, PadSize - (sizeof(T) % PadSize)> _padding;
    };

    // T whose size is already a multiple of PadSize: alignment alone suffices (a zero-sized
    // padding array would be ill-formed). Inherits T's constructors like _Padded.
    template <typename T, std::size_t PadSize>
    struct alignas(PadSize) _Padded0 : T {
        using T::T;
    };

    /// <summary>A <typeparamref name="T"/> that occupies whole cache lines of its own.</summary>
    /// <typeparam name="T">The wrapped type: a class that can be derived from.</typeparam>
    /// <typeparam name="PadSize">
    /// Alignment and size granularity in bytes; a power of two, by default the cache line size.
    /// </typeparam>
    /// <remarks>
    /// Also in arrays: aligned to <typeparamref name="PadSize"/> and padded to a multiple of it.
    /// The padded object is a T and is constructed and used like one. T must be a class that can
    /// be derived from (not <c>final</c>). PadSize must be a power of two; default is the cache line
    /// size (<c>CT_CACHE_LINE_SIZE</c>).
    /// </remarks>
    export template <typename T, std::size_t PadSize = CT_CACHE_LINE_SIZE>
    using Padded = std::conditional_t<sizeof(T) % PadSize == 0, _Padded0<T, PadSize>, _Padded<T, PadSize>>;
} // namespace ThinkMeta::ConcurrentTasks::Cpp