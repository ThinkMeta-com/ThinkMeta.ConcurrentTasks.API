//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Helper macros shared by the library's modules. Included in the global module fragment of a
// module (before "export module"), so the macros never leak into importers.
//
// Preliminary API: everything in this file is subject to change.
//

#pragma once

#include <new>

/// <summary>Marks a class that is only ever used as a base.</summary>
/// <remarks>
/// Its constructor does not set up a vtable of its own. Same definition as in Core.h, so including
/// both is harmless.
/// </remarks>
#define CT_NOVTABLE __declspec(novtable)
/// <summary>Cache line size used for padding and alignment.</summary>
/// <remarks>Guards against false sharing; 64 on x64.</remarks>
/// <seealso cref="CT_ALIGNED"/>
/// <seealso cref="ThinkMeta::ConcurrentTasks::Cpp::Padded"/>
#define CT_CACHE_LINE_SIZE std::hardware_destructive_interference_size
/// <summary>Aligns a type or member to its own cache line.</summary>
/// <seealso cref="CT_CACHE_LINE_SIZE"/>
#define CT_ALIGNED alignas(CT_CACHE_LINE_SIZE)
/// <summary>Breaks into the debugger (or ends the process if none is attached).</summary>
#define CT_DEBUGBREAK __debugbreak

/// <summary>Asserts a condition in debug builds.</summary>
/// <param name="cond">An expression convertible to <c>bool</c> that must be <c>true</c>; free of side effects.</param>
/// <remarks>
/// Debug builds: breaks into the debugger (<see cref="CT_DEBUGBREAK"/>) if <paramref name="cond"/>
/// is <c>false</c>. Release builds: cond is not evaluated, so it must have no side effects. Written
/// as a statement: <c>CT_ASSERT(cond);</c>
/// </remarks>
#ifdef _DEBUG
#    define CT_ASSERT(cond)      \
        do {                     \
            if (!(cond))         \
                CT_DEBUGBREAK(); \
        } while (false)
#else
#    define CT_ASSERT(cond)
#endif
