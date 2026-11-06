#pragma once

#include "pch.h"
#include "CppUnitTest.h"

#if defined(_WIN32)
#    define NOMINMAX
#    include <Windows.h>
#else
#    include <csignal>
#    include <pthread.h>

#    include "WindowsTestCompat.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Tests {

    // --- files for the I/O tests --------------------------------------------------------------------
    // Paths are std::filesystem::path: its c_str() is the platform's file name type (wchar_t on
    // Windows, char elsewhere), so it goes straight into the library's Platform::FileName.

    // A new, unused path in the temporary directory; the file is not created. Linux: $TMPDIR, else
    // /var/tmp, which lies on a disk: a /tmp in memory (tmpfs) may refuse unbuffered I/O (O_DIRECT).
    inline std::filesystem::path TempFilePath(const char* prefix)
    {
#if defined(_WIN32)
        const std::filesystem::path directory = std::filesystem::temp_directory_path();
#else
        const char*                 tmp       = std::getenv("TMPDIR");
        const std::filesystem::path directory = tmp != nullptr && *tmp != 0 ? tmp : "/var/tmp";
#endif
        static std::atomic<unsigned> counter{0};
        return directory / std::format("{}{:08x}-{}.tmp", prefix, std::random_device{}(), counter.fetch_add(1));
    }

    // Creates or empties the file and writes the bytes; data may be nullptr for an empty file.
    inline bool WriteTestFile(const std::filesystem::path& path, const void* data, std::size_t size)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (size > 0)
            file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        file.close();
        return !file.fail();
    }

    // The whole file; empty if it cannot be read.
    inline std::vector<std::uint8_t> ReadTestFile(const std::filesystem::path& path)
    {
        std::error_code   error;
        const auto        size = std::filesystem::file_size(path, error);
        std::ifstream     file(path, std::ios::binary);
        if (error || !file)
            return {};
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file)
            data.clear();
        return data;
    }

    // Cuts or extends the file to a size; also while the library holds it open for reading.
    inline bool ResizeTestFile(const std::filesystem::path& path, std::uint64_t size)
    {
        std::error_code error;
        std::filesystem::resize_file(path, size, error);
        return !error;
    }

    // Removes the file, if there is one.
    inline void DeleteTestFile(const std::filesystem::path& path)
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    // Ends the calling test as skipped where the asynchronous I/O cannot start: on Linux when
    // io_uring is blocked (some containers and sandboxes); Windows always has it. Call it first.
    inline void SkipWithoutAsyncIo()
    {
#if !defined(_WIN32)
        static const bool available = [] {
            const bool enabled = AsyncIo::EnableAsyncIo();
            if (enabled)
                AsyncIo::DisableAsyncIo();
            return enabled;
        }();
        if (!available) {
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage("asynchronous I/O (io_uring) is not available here");
            throw Microsoft::VisualStudio::CppUnitTestFramework::TestSkipped{};
        }
#endif
    }

    // The scheduler implementations live in the core DLL and are only reachable through the C API.
    // This guard creates one for the scope and offers the same member names as the classes did, so
    // a test reads the same as before.
    class ScopedScheduler
    {
    public:
        explicit ScopedScheduler(const SchedulerInfo& info, SchedulerKind kind = SchedulerKind::Default) noexcept :
            scheduler_(CreateTaskScheduler(kind, &info))
        {
            Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsNotNull(scheduler_, L"CreateTaskScheduler failed");
        }

        ~ScopedScheduler() noexcept
        {
            if (scheduler_)
                scheduler_->Destroy();
        }

        ScopedScheduler(const ScopedScheduler&)            = delete;
        ScopedScheduler(ScopedScheduler&&)                 = delete;
        ScopedScheduler& operator=(const ScopedScheduler&) = delete;
        ScopedScheduler& operator=(ScopedScheduler&&)      = delete;

        void AddTask(ITask* task, TaskPriority priority = TaskPriority::Default) noexcept { scheduler_->AddTask(task, priority); }
        void Start() noexcept { scheduler_->Start(); }
        void Stop() noexcept { scheduler_->Stop(); }
        void SuspendTask(ISuspendCallback* callback) noexcept { scheduler_->SuspendTask(callback); }
        void YieldTask(YieldFlags yieldFlags = YieldFlags::Default) noexcept { scheduler_->YieldTask(yieldFlags); }
        void ResumeTask(SuspendedTask suspendedTask) noexcept { scheduler_->ResumeTask(suspendedTask); }
        ITask* StealTaskOrSuspend(TaskPriority& taskPriority) noexcept { return scheduler_->StealTaskOrSuspend(taskPriority); }

        ITaskScheduler* Get() const noexcept { return scheduler_; }

    private:
        ITaskScheduler* scheduler_;
    };

    using ActionTask = Tasks::LambdaTask<std::function<void()>>;

    inline std::vector<DWORD> GetTestThreadCounts()
    {
        const DWORD cpuCount = std::max(1u, std::thread::hardware_concurrency());
        if (cpuCount > 1) {
            return {1, cpuCount};
        }
        return {1};
    }

    inline std::vector<DWORD> GetMultiThreadTestCounts()
    {
        const DWORD cpuCount = std::max(1u, std::thread::hardware_concurrency());
        if (cpuCount > 2) {
            return {2, cpuCount};
        }
        return {2};
    }

    inline void LogDebugMessage(const std::wstring& message) noexcept
    {
        std::wstring line = message + L"\n";
        ::OutputDebugStringW(line.c_str());
        Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(message.c_str());
    }

    template <typename Rep, typename Period>
    inline void SimulateCpuLoad(std::chrono::duration<Rep, Period> duration) noexcept
    {
        const auto start = std::chrono::high_resolution_clock::now();
        volatile std::uint64_t dummy = 0;
        while (std::chrono::high_resolution_clock::now() - start < duration) {
            dummy += 1;
            YieldProcessor();
        }
    }

    template <typename Predicate>
    inline bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate())
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return predicate();
    }

    // Cooperative variant of WaitFor for use INSIDE a scheduler task. It yields the
    // current fiber back to the scheduler instead of blocking the underlying OS
    // thread with sleep_for, so other tasks on the same worker can make progress.
    template <typename Predicate>
    inline bool YieldWaitFor(Predicate predicate) noexcept
    {
        while (!predicate())
            GetTaskScheduler()->YieldTask();

        return true;
    }

#if defined(_WIN32)
    struct SchedulerThreadTracker {
        std::vector<HANDLE> threadHandles;
        std::mutex          mutex;

        static void ThreadCallback([[maybe_unused]] ITaskScheduler* scheduler, std::uint32_t threadIndex, NativeThread thread, void* context, ThreadCallbackReason reason) noexcept
        {
            auto* tracker = static_cast<SchedulerThreadTracker*>(context);
            if (reason == ThreadCallbackReason::Starting) {
                HANDLE hThread = nullptr;
                BOOL   res     = ::DuplicateHandle(::GetCurrentProcess(), thread, ::GetCurrentProcess(), &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
                if (res && hThread) {
                    std::lock_guard<std::mutex> lock(tracker->mutex);
                    if (threadIndex >= tracker->threadHandles.size()) {
                        tracker->threadHandles.resize(threadIndex + 1, nullptr);
                    }
                    tracker->threadHandles[threadIndex] = hThread;
                }
            }
        }

        void SuspendAll()
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (HANDLE h : threadHandles) {
                if (h) {
                    ::SuspendThread(h);
                }
            }
        }

        void ResumeAll()
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (HANDLE h : threadHandles) {
                if (h) {
                    ::ResumeThread(h);
                }
            }
        }

        void CloseHandles()
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (HANDLE h : threadHandles) {
                if (h) {
                    ::CloseHandle(h);
                }
            }
            threadHandles.clear();
        }
    };
#else
    // Linux has no SuspendThread: a signal stops a thread instead. Its handler counts the thread as
    // held and keeps it until ResumeAll opens the gate; SuspendAll returns once every tracked thread
    // is held, as SuspendThread returns once its thread is stopped.
    namespace Detail {
        inline std::atomic<bool> holdThreads{false};
        inline std::atomic<int>  heldThreads{0};

        inline void HoldThread(int) noexcept
        {
            heldThreads.fetch_add(1);
            while (holdThreads.load())
                sched_yield();
            heldThreads.fetch_sub(1);
        }
    } // namespace Detail

    struct SchedulerThreadTracker {
        std::vector<pthread_t> threads;
        std::mutex             mutex;

        static void ThreadCallback([[maybe_unused]] ITaskScheduler* scheduler, std::uint32_t threadIndex, NativeThread thread, void* context, ThreadCallbackReason reason) noexcept
        {
            auto* tracker = static_cast<SchedulerThreadTracker*>(context);
            if (reason == ThreadCallbackReason::Starting) {
                std::lock_guard<std::mutex> lock(tracker->mutex);
                if (threadIndex >= tracker->threads.size())
                    tracker->threads.resize(threadIndex + 1, pthread_t{});
                tracker->threads[threadIndex] = thread;
            }
        }

        void SuspendAll()
        {
            std::lock_guard<std::mutex> lock(mutex);

            struct sigaction action{};
            action.sa_handler = &Detail::HoldThread;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART;
            sigaction(SIGUSR1, &action, nullptr);

            Detail::holdThreads.store(true);
            int count = 0;
            for (pthread_t thread : threads) {
                if (thread && pthread_kill(thread, SIGUSR1) == 0)
                    ++count;
            }
            while (Detail::heldThreads.load() < count)
                sched_yield();
        }

        void ResumeAll()
        {
            std::lock_guard<std::mutex> lock(mutex);
            Detail::holdThreads.store(false);
            while (Detail::heldThreads.load() > 0)
                sched_yield();
        }

        void CloseHandles()
        {
            std::lock_guard<std::mutex> lock(mutex);
            threads.clear();
        }
    };
#endif

} // namespace ThinkMeta::ConcurrentTasks::Tests
