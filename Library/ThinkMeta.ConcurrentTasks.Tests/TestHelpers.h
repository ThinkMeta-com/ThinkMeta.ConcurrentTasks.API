#pragma once

#include "pch.h"
#include "CppUnitTest.h"

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace ThinkMeta::ConcurrentTasks::Tests {

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

    struct SchedulerThreadTracker {
        std::vector<HANDLE> threadHandles;
        std::mutex          mutex;

        static void ThreadCallback([[maybe_unused]] ITaskScheduler* scheduler, DWORD threadIndex, HANDLE thread, void* context, ThreadCallbackReason reason) noexcept
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

} // namespace ThinkMeta::ConcurrentTasks::Tests
