# ThinkMeta.ConcurrentTasks on Linux (x64, clang, Ninja): the library against the prebuilt core
# in Core/bin/linux-x64, its unit tests and the samples that run on Linux. Windows builds use the
# .slnx and .vcxproj files; this file never takes part in them.
#
#   cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux-release
#   ctest --test-dir build/linux-release
#
# (build/ holds the Windows builds as well, in build/Release and build/Debug.)
#
# Projects of your own use cmake/ThinkMeta.ConcurrentTasksConfig.cmake through find_package.

cmake_minimum_required(VERSION 3.28)

# Without a build type: a release build, as the core is one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(ThinkMeta.ConcurrentTasks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

# -Wall -Wextra always; as errors on request (a newer clang may warn about more than the one the
# code was checked with).
option(CT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${CT_WARNINGS_AS_ERRORS}>>:-Werror>
)

# the prebuilt core and the library, as a project of your own gets them
find_package(ThinkMeta.ConcurrentTasks CONFIG REQUIRED PATHS "${CMAKE_CURRENT_SOURCE_DIR}/cmake" NO_DEFAULT_PATH)

option(CT_BUILD_TESTS "Build the unit tests" ON)
if(CT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Library/ThinkMeta.ConcurrentTasks.Tests tests)
endif()

option(CT_BUILD_SAMPLES "Build the samples that run on Linux" ON)
if(CT_BUILD_SAMPLES)
    enable_testing()
    add_subdirectory(Samples samples)
endif()
