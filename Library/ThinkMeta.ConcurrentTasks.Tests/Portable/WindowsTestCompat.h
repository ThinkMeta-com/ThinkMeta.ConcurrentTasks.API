//
// Copyright (c) 2026 ThinkMeta Software GmbH
//
// The few Windows API calls the unit tests make, for Linux: Sleep, plain threads and events with
// WaitForSingleObject / WaitForMultipleObjects, the processor count, GetTickCount64 and the performance counter,
// VirtualAlloc / VirtualFree of whole pages, and LoadLibrary / GetProcAddress for the core's
// exports. The tests include it in place of Windows.h,
// so their sources stay the same on both platforms. Test code only: the library and the core have
// their own platform layers and never see this header.
//

#pragma once

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

using DWORD     = std::uint32_t;
using ULONGLONG = unsigned long long;
using BOOL    = int;
using LPVOID  = void*;
using HMODULE = void*;
using FARPROC = void*;

#define WINAPI
#ifndef TRUE
#    define TRUE 1
#endif
#ifndef FALSE
#    define FALSE 0
#endif
#define INFINITE             0xFFFFFFFFu
#define WAIT_OBJECT_0        0u
#define WAIT_TIMEOUT         258u
#define ALL_PROCESSOR_GROUPS 0xFFFF

using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);

namespace WindowsTestCompat {
    // What a HANDLE stands for here: a thread or an event, both waitable.
    class Object
    {
    public:
        virtual ~Object() = default;

        // true once signaled, false after the timeout
        bool Wait(DWORD milliseconds)
        {
            std::unique_lock lock(mutex_);
            const auto       signaled = [this] { return signaled_; };
            if (milliseconds == INFINITE)
                condition_.wait(lock, signaled);
            else if (!condition_.wait_for(lock, std::chrono::milliseconds(milliseconds), signaled))
                return false;
            Consume();
            return true;
        }

    protected:
        void Signal(bool value)
        {
            {
                std::lock_guard lock(mutex_);
                signaled_ = value;
            }
            if (value)
                condition_.notify_all();
        }

        // what a successful wait does to the state: nothing, or reset it (auto-reset event)
        virtual void Consume() {}

        bool signaled_ = false;

    private:
        std::mutex              mutex_;
        std::condition_variable condition_;
    };

    class Thread final : public Object
    {
    public:
        Thread(LPTHREAD_START_ROUTINE start, LPVOID parameter) : start_(start), parameter_(parameter)
        {
            started_ = pthread_create(&thread_, nullptr, &Run, this) == 0;
        }

        ~Thread() override
        {
            // CloseHandle does not wait on Windows; the tests always wait first, so join is free
            if (started_)
                pthread_join(thread_, nullptr);
        }

        bool Started() const noexcept { return started_; }

    private:
        static void* Run(void* argument)
        {
            auto* self = static_cast<Thread*>(argument);
            self->start_(self->parameter_);
            self->Signal(true);
            return nullptr;
        }

        LPTHREAD_START_ROUTINE start_;
        LPVOID                 parameter_;
        pthread_t              thread_{};
        bool                   started_ = false;
    };

    class Event final : public Object
    {
    public:
        Event(bool manualReset, bool initialState) : manualReset_(manualReset) { signaled_ = initialState; }

        void Set() { Signal(true); }
        void Reset() { Signal(false); }

    private:
        void Consume() override
        {
            if (!manualReset_)
                signaled_ = false;
        }

        bool manualReset_;
    };
} // namespace WindowsTestCompat

using HANDLE = WindowsTestCompat::Object*;

inline void Sleep(DWORD milliseconds)
{
    timespec duration{static_cast<time_t>(milliseconds / 1000), static_cast<long>(milliseconds % 1000) * 1000000};
    while (nanosleep(&duration, &duration) != 0) {}
}

inline void YieldProcessor() noexcept
{
    __builtin_ia32_pause();
}

inline BOOL SwitchToThread() noexcept
{
    return sched_yield() == 0;
}

inline DWORD GetCurrentThreadId() noexcept
{
    return static_cast<DWORD>(gettid());
}

inline DWORD GetActiveProcessorCount([[maybe_unused]] unsigned short group) noexcept
{
    cpu_set_t set;
    return sched_getaffinity(0, sizeof(set), &set) == 0 ? static_cast<DWORD>(CPU_COUNT(&set)) : 1;
}

inline void OutputDebugStringW([[maybe_unused]] const wchar_t* text) noexcept {}

// --- threads and events ---

inline HANDLE CreateThread(void*, std::size_t, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD, DWORD*)
{
    auto* thread = new WindowsTestCompat::Thread(start, parameter);
    if (thread->Started())
        return thread;
    delete thread;
    return nullptr;
}

inline HANDLE CreateEventW(void*, BOOL manualReset, BOOL initialState, const wchar_t*)
{
    return new WindowsTestCompat::Event(manualReset != FALSE, initialState != FALSE);
}

inline BOOL SetEvent(HANDLE event)
{
    static_cast<WindowsTestCompat::Event*>(event)->Set();
    return TRUE;
}

inline BOOL ResetEvent(HANDLE event)
{
    static_cast<WindowsTestCompat::Event*>(event)->Reset();
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds)
{
    return object->Wait(milliseconds) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

// Only what the tests use: wait for all, without a timeout.
inline DWORD WaitForMultipleObjects(DWORD count, const HANDLE* objects, [[maybe_unused]] BOOL waitAll, DWORD milliseconds)
{
    for (DWORD i = 0; i < count; ++i)
        objects[i]->Wait(milliseconds);
    return WAIT_OBJECT_0;
}

inline BOOL CloseHandle(HANDLE object)
{
    delete object;
    return TRUE;
}

// --- whole pages ---

#define MEM_COMMIT     0x00001000u
#define MEM_RESERVE    0x00002000u
#define MEM_RELEASE    0x00008000u
#define PAGE_READWRITE 0x04u

namespace WindowsTestCompat {
    // munmap needs the size VirtualFree is not told: kept per allocation
    inline std::mutex                              mappingsLock;
    inline std::unordered_map<void*, std::size_t> mappings;
} // namespace WindowsTestCompat

// Only what the tests use: reserve and commit at once, read-write; freed with MEM_RELEASE, after
// which any access faults, as on Windows.
inline void* VirtualAlloc(void* address, std::size_t size, [[maybe_unused]] DWORD type, [[maybe_unused]] DWORD protection)
{
    void* pages = mmap(address, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED)
        return nullptr;
    std::lock_guard lock(WindowsTestCompat::mappingsLock);
    WindowsTestCompat::mappings[pages] = size;
    return pages;
}

inline BOOL VirtualFree(void* address, [[maybe_unused]] std::size_t size, [[maybe_unused]] DWORD type)
{
    std::size_t bytes;
    {
        std::lock_guard lock(WindowsTestCompat::mappingsLock);
        auto            found = WindowsTestCompat::mappings.find(address);
        if (found == WindowsTestCompat::mappings.end())
            return FALSE;
        bytes = found->second;
        WindowsTestCompat::mappings.erase(found);
    }
    return munmap(address, bytes) == 0;
}

// --- clocks ---

// Milliseconds since an arbitrary start, monotonic.
inline ULONGLONG GetTickCount64() noexcept
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<ULONGLONG>(now.tv_sec) * 1000 + static_cast<ULONGLONG>(now.tv_nsec) / 1000000;
}

// --- performance counter ---

struct LARGE_INTEGER {
    long long QuadPart;
};

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = static_cast<long long>(now.tv_sec) * 1000000000 + now.tv_nsec;
    return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept
{
    frequency->QuadPart = 1000000000;
    return TRUE;
}

// --- the core library ---

namespace WindowsTestCompat {
    // "Name.dll" is libName.so.2 here (2 is the ABI version, the core's SONAME).
    inline std::string SharedObjectName(const wchar_t* dllName)
    {
        std::string name;
        for (; *dllName; ++dllName)
            name += static_cast<char>(*dllName);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".dll") == 0)
            name.resize(name.size() - 4);
        return "lib" + name + ".so.2";
    }
} // namespace WindowsTestCompat

inline HMODULE LoadLibraryW(const wchar_t* name)
{
    return dlopen(WindowsTestCompat::SharedObjectName(name).c_str(), RTLD_NOW);
}

// The module only if it is loaded already, as on Windows.
inline HMODULE GetModuleHandleW(const wchar_t* name)
{
    void* module = dlopen(WindowsTestCompat::SharedObjectName(name).c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (module)
        dlclose(module); // GetModuleHandle takes no reference; the program keeps the library loaded
    return module;
}

inline FARPROC GetProcAddress(HMODULE module, const char* name)
{
    return dlsym(module, name);
}

inline BOOL FreeLibrary(HMODULE module)
{
    return dlclose(module) == 0;
}
