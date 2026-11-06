# ThinkMeta.ConcurrentTasks for CMake projects on Linux (x64, clang, Ninja). Preliminary and
# subject to change.
#
#   find_package(ThinkMeta.ConcurrentTasks CONFIG REQUIRED PATHS <this repository>/cmake)
#   target_link_libraries(my_program PRIVATE ThinkMeta::ConcurrentTasks)
#
# Gives two targets:
#   ThinkMeta::Core             the prebuilt core, Core/bin/linux-x64/libThinkMeta.ConcurrentTasks.Core.so.2,
#                               with Include/ for Core.h
#   ThinkMeta::ConcurrentTasks  the library's C++ modules, built from source as part of the calling
#                               project; links the core, so a program needs nothing else
#
# Windows projects use Core/ThinkMeta.ConcurrentTasks.Core.props instead.

if(CMAKE_VERSION VERSION_LESS 3.28)
    message(FATAL_ERROR "ThinkMeta.ConcurrentTasks needs CMake 3.28 or later (C++ modules)")
endif()

# once per build, however often find_package is called
include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "ThinkMeta.ConcurrentTasks: this file is for Linux on x64; Windows uses Core/ThinkMeta.ConcurrentTasks.Core.props")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ThinkMeta.ConcurrentTasks: the library builds with clang (19 or later) only")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "ThinkMeta.ConcurrentTasks: C++ modules need the Ninja generator (cmake -G Ninja)")
endif()

get_filename_component(_ctRoot "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_ctCore "${_ctRoot}/Core/bin/linux-x64/libThinkMeta.ConcurrentTasks.Core.so.2")
if(NOT EXISTS "${_ctCore}")
    message(FATAL_ERROR "ThinkMeta.ConcurrentTasks: the prebuilt core is missing (${_ctCore})")
endif()

# The core: one file under its SONAME (git keeps no symlinks on Windows checkouts).
add_library(ThinkMeta.ConcurrentTasks.Core SHARED IMPORTED GLOBAL)
set_target_properties(ThinkMeta.ConcurrentTasks.Core PROPERTIES
    IMPORTED_LOCATION "${_ctCore}"
    IMPORTED_SONAME libThinkMeta.ConcurrentTasks.Core.so.2
    INTERFACE_INCLUDE_DIRECTORIES "${_ctRoot}/Include"
)
add_library(ThinkMeta::Core ALIAS ThinkMeta.ConcurrentTasks.Core)

# The library, from source: C++ modules are compiled with the flags of the project that imports them.
find_package(Threads REQUIRED)
add_subdirectory("${_ctRoot}/Library/ThinkMeta.ConcurrentTasks" "${CMAKE_BINARY_DIR}/ThinkMeta.ConcurrentTasks")
target_link_libraries(ThinkMeta.ConcurrentTasks INTERFACE ThinkMeta.ConcurrentTasks.Core Threads::Threads)

set(ThinkMeta.ConcurrentTasks_FOUND TRUE)
unset(_ctRoot)
unset(_ctCore)
