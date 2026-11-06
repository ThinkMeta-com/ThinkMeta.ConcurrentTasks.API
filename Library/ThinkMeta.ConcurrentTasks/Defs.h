//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// Helper macros shared by the library's modules and the core: the platform and compiler the code
// is built for, and the few compiler-specific operations they need. Included in the global module
// fragment of a module (before "export module"), so the macros never leak into importers.
//
// Everything below is a single instruction or a compile-time constant on every platform; nothing
// here may cost a function call on a hot path.
//
// Preliminary API: everything in this file is subject to change.
//

#pragma once

/// <summary>The target platform: exactly one of CT_PLATFORM_WINDOWS and CT_PLATFORM_LINUX is defined.</summary>
#if defined(_WIN32)
#    define CT_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#    define CT_PLATFORM_LINUX 1
#else
#    error "ThinkMeta.ConcurrentTasks: unsupported platform"
#endif

/// <summary>The compiler: exactly one of CT_COMPILER_MSVC and CT_COMPILER_CLANG is defined.</summary>
/// <remarks>clang-cl defines both _MSC_VER and __clang__ and counts as clang.</remarks>
#if defined(__clang__)
#    define CT_COMPILER_CLANG 1
#elif defined(_MSC_VER)
#    define CT_COMPILER_MSVC 1
#else
#    error "ThinkMeta.ConcurrentTasks: unsupported compiler"
#endif

#if defined(CT_COMPILER_MSVC)
#    include <intrin.h>
#elif defined(CT_COMPILER_CLANG)
#    include <immintrin.h>
#endif

/// <summary>Marks a class that is only ever used as a base.</summary>
/// <remarks>
/// With MSVC its constructor does not set up a vtable of its own; other compilers have no
/// equivalent. Same definition as in Core.h, so including both is harmless.
/// </remarks>
#ifndef CT_NOVTABLE
#    if defined(CT_COMPILER_MSVC)
#        define CT_NOVTABLE __declspec(novtable)
#    else
#        define CT_NOVTABLE
#    endif
#endif

/// <summary>Cache line size used for padding and alignment.</summary>
/// <remarks>
/// Guards against false sharing. A fixed 64, not std::hardware_destructive_interference_size: the
/// value is part of the ABI (queue nodes, per-thread data), and the standard libraries do not agree
/// on the constant.
/// </remarks>
/// <seealso cref="CT_ALIGNED"/>
/// <seealso cref="ThinkMeta::ConcurrentTasks::Cpp::Padded"/>
#define CT_CACHE_LINE_SIZE 64
/// <summary>Aligns a type or member to its own cache line.</summary>
/// <seealso cref="CT_CACHE_LINE_SIZE"/>
#define CT_ALIGNED alignas(CT_CACHE_LINE_SIZE)

/// <summary>Breaks into the debugger (or ends the process if none is attached).</summary>
#if defined(CT_COMPILER_MSVC)
#    define CT_DEBUGBREAK __debugbreak
#else
#    define CT_DEBUGBREAK __builtin_debugtrap
#endif

/// <summary>Ends the process at once, without unwinding or cleanup.</summary>
/// <remarks>
/// For a state the code cannot go on from: out of memory where there is no way to report it, a
/// stack that could not be created. Windows: <c>__fastfail</c> with <c>FAST_FAIL_FATAL_APP_EXIT</c>
/// (the including file has Windows.h); elsewhere a trap instruction.
/// </remarks>
#if defined(CT_PLATFORM_WINDOWS)
#    define CT_FASTFAIL() __fastfail(FAST_FAIL_FATAL_APP_EXIT)
#else
#    define CT_FASTFAIL() __builtin_trap()
#endif

// x86-64 only: CT_PAUSE, the 16-byte compare-exchange (Cpp/Atomic128.ixx, -mcx16) and the fiber
// switch of the core assume it.
#if !defined(_M_X64) && !defined(__x86_64__)
#    error ThinkMeta.ConcurrentTasks supports x86-64 only
#endif

/// <summary>Tells the processor that the caller spins: <c>pause</c> on x86-64.</summary>
#if defined(CT_COMPILER_MSVC)
#    define CT_PAUSE() _mm_pause()
#else
#    define CT_PAUSE() __builtin_ia32_pause()
#endif

/// <summary>Asserts a condition in debug builds.</summary>
/// <param name="cond">An expression convertible to <c>bool</c> that must be <c>true</c>; free of side effects.</param>
/// <remarks>
/// Debug builds: breaks into the debugger (<see cref="CT_DEBUGBREAK"/>) if <paramref name="cond"/>
/// is <c>false</c>. Release builds: cond is not evaluated, so it must have no side effects. Written
/// as a statement: <c>CT_ASSERT(cond);</c>
/// A debug build is one with <c>_DEBUG</c> (MSVC) or without <c>NDEBUG</c> (other compilers).
/// </remarks>
#if defined(_DEBUG) || (!defined(CT_COMPILER_MSVC) && !defined(NDEBUG))
#    define CT_ASSERT(cond)      \
        do {                     \
            if (!(cond))         \
                CT_DEBUGBREAK(); \
        } while (false)
#else
#    define CT_ASSERT(cond)
#endif
