# The open library, for Linux (clang, Ninja). Windows builds use the .vcxproj next to this file;
# this file never takes part in them.
#
# Built through the core's CMakeLists.txt (Core\Tools\Linux\Run-Linux.ps1 -Target library):
# clang 20 on Linux compiles every module listed below.
#
# The modules are C++ named modules in .ixx files; CMake needs the language of each told, since
# .ixx is not an extension it knows.

cmake_minimum_required(VERSION 3.28)

add_library(ThinkMeta.ConcurrentTasks STATIC)
add_library(ThinkMeta::ConcurrentTasks ALIAS ThinkMeta.ConcurrentTasks)

set(CT_LIBRARY_MODULES
    Cpp/Padded.ixx
    Cpp/Atomic128.ixx
    Platform/Clock.ixx
    Platform/PlatformThread.ixx
    Platform/PlatformErrorException.ixx
    Posix/PosixError.ixx
    Posix/PosixVirtualMemory.ixx
    Platform/PlatformError.ixx
    Platform/PlatformVirtualMemory.ixx
    Platform/PlatformMutex.ixx
    Platform/PlatformFileOptions.ixx
    Posix/PosixFile.ixx
    Platform/PlatformFile.ixx
    Posix/PosixSocket.ixx
    Platform/PlatformSocket.ixx
    Net/IpSocket.ixx
    Net/TcpSocket.ixx
    Collections/BasicStack.ixx
    Collections/ConcurrentStack.ixx
    Collections/BoundedPointerQueue.ixx
    Memory/SequentialMemory.ixx
    Memory/SpscVirtualMemoryPipe.ixx
    Synchronization/Suspendable.ixx
    Synchronization/SuspendableBase.ixx
    Synchronization/Waitable.ixx
    Synchronization/AutoResetEvent.ixx
    Synchronization/ManualResetEvent.ixx
    Synchronization/Semaphore.ixx
    Synchronization/CriticalSection.ixx
    Synchronization/TaskGroup.ixx
    Synchronization/CallbackAwait.ixx
    Tasks/Task.ixx
    Tasks/WaitableTask.ixx
    Tasks/RunAndWait.ixx
    AsyncIo/AsyncIoRequest.ixx
    AsyncIo/AsyncFileIoRequest.ixx
    AsyncIo/AsyncFile.ixx
    AsyncIo/StreamedFileReader.ixx
    AsyncIo/OrderedFileWriter.ixx
    AsyncIo/AsyncSocketIoRequest.ixx
    AsyncIo/AsyncSocketPosix.ixx # the module AsyncIo.AsyncSocket; AsyncSocket.ixx is the Windows one
    AsyncIo/AsyncIpSocket.ixx
    AsyncIo/AsyncTcpSocket.ixx
)

# Not on Linux: Windows/* (Posix/* stands in for the parts portable code needs), AsyncIo/AsyncSocket.ixx
# (Winsock; AsyncSocketPosix.ixx is the same module here), Schedulers/DispatcherScheduler.ixx (HWND),
# WinRT/*, MediaFoundation/*.

set_source_files_properties(${CT_LIBRARY_MODULES} PROPERTIES LANGUAGE CXX)

target_sources(ThinkMeta.ConcurrentTasks
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES ${CT_LIBRARY_MODULES}
)

target_include_directories(ThinkMeta.ConcurrentTasks
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/../../Include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(ThinkMeta.ConcurrentTasks PUBLIC cxx_std_23)

# clang only (see the plan): 16-byte atomics are lock-free with -mcx16, GCC would call libatomic.
target_compile_options(ThinkMeta.ConcurrentTasks PUBLIC -mcx16)
