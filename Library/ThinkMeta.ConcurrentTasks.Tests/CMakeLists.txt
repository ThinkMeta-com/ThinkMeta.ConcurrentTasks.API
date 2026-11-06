# The unit tests for Linux (clang, Ninja): the same sources as the MSTest project next to this file,
# with the test shim in Portable/ (CppUnitTest.h stands in for MSTest, WindowsTestCompat.h for the
# few Windows calls the tests make). Windows builds use the .vcxproj; this file never takes part in
# them.
#
# Used by the API's own CMakeLists.txt (against the prebuilt core) and by the core's (against the
# core it builds). Either way the core is the target ThinkMeta.ConcurrentTasks.Core.

# The test shim on its own: needs neither the core nor the library.
add_executable(ThinkMeta.ConcurrentTasks.ShimTests Portable/ShimTests.cpp Portable/TestMain.cpp)
target_include_directories(ThinkMeta.ConcurrentTasks.ShimTests PRIVATE Portable)
set_target_properties(ThinkMeta.ConcurrentTasks.ShimTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME ThinkMeta.ConcurrentTasks.ShimTests COMMAND ThinkMeta.ConcurrentTasks.ShimTests)

# One test program from the given sources and the shim's main. The programs land in the top build
# directory, wherever this file is included from.
function(ct_add_tests name directory)
    set(testsDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    add_executable(${name} ${ARGN} ${testsDir}/Portable/TestMain.cpp)
    target_include_directories(${name} PRIVATE ${testsDir}/Portable ${directory} ${testsDir})
    # The core is linked as a file, not as a target: the core build compiles some of the library's
    # modules into the core (without exceptions), and CMake would hand those BMIs to every target
    # that links it - next to the library's own builds of the same modules. A translation unit that
    # then sees two builds of one module fails (ambiguous names, clang crashes).
    target_link_libraries(${name} PRIVATE ThinkMeta::ConcurrentTasks $<TARGET_LINKER_FILE:ThinkMeta.ConcurrentTasks.Core> pthread)
    ct_depend_on_core(${name})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        BUILD_RPATH $<TARGET_FILE_DIR:ThinkMeta.ConcurrentTasks.Core>
    )
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()

# Builds the core first when it is built here; a prebuilt core (IMPORTED) needs nothing.
function(ct_depend_on_core name)
    get_target_property(imported ThinkMeta.ConcurrentTasks.Core IMPORTED)
    if(NOT imported)
        add_dependencies(${name} ThinkMeta.ConcurrentTasks.Core)
    endif()
endfunction()

option(CT_BUILD_UNIT_TESTS "Build the unit tests (needs the core)" ON)

if(CT_BUILD_UNIT_TESTS AND TARGET ThinkMeta.ConcurrentTasks.Core)
    ct_add_tests(ThinkMeta.ConcurrentTasks.Tests ${CMAKE_CURRENT_SOURCE_DIR}
        AutoResetEventTests.cpp
        ManualResetEventTests.cpp
        SemaphoreTests.cpp
        CriticalSectionTests.cpp
        TaskGroupTests.cpp
        ObjectLifetimeTests.cpp
        TimerTests.cpp
        SchedulerStarvationTests.cpp
        TaskSwapSchedulerTests.cpp
        WaitableLambdaTaskTests.cpp
        SpscVirtualMemoryPipeTests.cpp
        PlatformFileTests.cpp
        AsyncFileTests.cpp
        PlatformSocketTests.cpp
        AsyncSocketPosixTests.cpp # Linux only: AsyncSocket below AsyncTcpSocket
        AsyncIoTests.cpp
        StreamedFileIoTests.cpp
        AsyncSocketTests.cpp
        # Windows only: DispatcherSchedulerTests, AwaitTests, WinRTAwaitTests, CrashDump
    )
endif()
