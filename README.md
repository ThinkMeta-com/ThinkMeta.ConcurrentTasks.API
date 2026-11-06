# ThinkMeta.ConcurrentTasks

A task scheduler for Windows and Linux (x64) that runs tasks on fibers: a task can block on a
synchronization primitive or on asynchronous I/O without blocking its OS thread, so a handful of
threads keep working while thousands of tasks wait.

> **Preliminary API.** Everything in this repository - the core ABI in `Core.h` as well as the
> library modules - is preliminary and subject to change.

**Performance first.** The core and the library are built for speed, at the price of not being
watertight against misuse. Correct use is the developer's responsibility: an object that tasks
wait on must outlive every task that can still wait on it, a bounded queue must be sized for the
use case, a single-waiter primitive (`_s`) must have at most one waiter at a time. Debug builds
assert some of these rules; release builds do not check them.

**Documentation:** introduction, concepts, guides and the API reference are at
[www.thinkmeta.com/concurrenttasks](https://www.thinkmeta.com/concurrenttasks).

## Contents

| Folder | Contents |
|---|---|
| `Include\ThinkMeta.ConcurrentTasks\Core.h` | the ABI: the scheduler's C entry points and the interfaces exchanged with it |
| `Library\ThinkMeta.ConcurrentTasks` | C++ modules on top of it: tasks, synchronization primitives, collections, virtual-memory pipes, asynchronous files and sockets |
| `Library\ThinkMeta.ConcurrentTasks.Tests` | tests of that public surface |
| `Samples` | small programs showing how it is used; `Samples\Docs` holds the samples of the online documentation |
| `Core` | the prebuilt scheduler core (release build, x64) for Windows and Linux, plus the .props file that binds it into a Visual Studio project |
| `cmake` | the CMake package configuration for Linux projects |

The scheduler core ships as a binary:

- Windows: `ThinkMeta.ConcurrentTasks.Core.dll` (about 30 KB), importing nothing but KERNEL32 and
  the API set behind `WaitOnAddress`.
- Linux: `libThinkMeta.ConcurrentTasks.Core.so.2` (about 48 KB), needing nothing but the C library.

The same release build serves debug and release builds of your program. Larger programs built on
ThinkMeta.ConcurrentTasks - parallel MP3, Opus, Deflate and JPEG encoders for Windows - are in
[ThinkMeta.ConcurrentTasks.Samples](https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.Samples).

```
git clone --recurse-submodules https://github.com/ThinkMeta-com/ThinkMeta.ConcurrentTasks.API.git
```

## Building on Windows

Requirements: Windows 10 or newer, x64, Visual Studio 2026 with the C++ desktop workload.

Open `ThinkMeta.ConcurrentTasks.API.slnx` in Visual Studio 2026, or:

```
msbuild ThinkMeta.ConcurrentTasks.API.slnx /p:Configuration=Release /p:Platform=x64
```

`Core\ThinkMeta.ConcurrentTasks.Core.props` adds the include directory, links the import library and
copies the DLL next to the output; a project only has to import it after `Microsoft.Cpp.props`.

## Building on Linux

Requirements: x64, glibc 2.34 or newer, clang 19 or newer (tested with clang 20), CMake 3.28 or
newer and Ninja. Asynchronous I/O runs on io_uring; Linux 5.19 or newer is recommended. Where
io_uring is not available, `EnableAsyncIo` returns `false`; the scheduler, the timers and the
synchronization primitives work all the same.

The library, its tests and the samples that run on Linux:

```
cmake -S . -B build/linux-release -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux-release
ctest --test-dir build/linux-release
```

In a project of your own, `cmake\ThinkMeta.ConcurrentTasksConfig.cmake` brings in the prebuilt
core and builds the library's modules as part of the project:

```cmake
find_package(ThinkMeta.ConcurrentTasks CONFIG REQUIRED PATHS <this repository>/cmake)
target_link_libraries(my_program PRIVATE ThinkMeta::ConcurrentTasks)
```

## Samples

| Sample | What it shows |
|---|---|
| `PingPong` | two tasks handing the processor back and forth through semaphores, ten million times |
| `Hashing` | reading a file asynchronously and hashing it in a second task, connected by a virtual-memory pipe (Windows) |
| `Docs\*` | the small programs of the online documentation's guides; on Linux all but `BluetoothDevices`, `GuideUiThread`, `GuideThreadCallbacks` and `Hashing` |

## License

Everything in this repository except the `Core` folder's binaries is under the MIT license
(`LICENSE`). The core binaries - the DLL, its import library and the Linux shared library - are
proprietary: free for private and non-commercial use, benchmarks of any kind explicitly permitted;
see `Core\LICENSE-Core.txt` and `Core\redist.txt`. Third-party notices are in
`THIRD-PARTY-NOTICES.md`.
