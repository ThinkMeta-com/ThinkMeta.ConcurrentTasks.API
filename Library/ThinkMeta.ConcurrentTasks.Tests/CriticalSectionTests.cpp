#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.CriticalSection;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    constexpr std::size_t NumTasks                 = 32;
    constexpr auto        TaskExecutionDuration    = std::chrono::milliseconds(500);
    constexpr DWORD       SleepSafetyMarginSeconds = 2;

    template <typename CS_T>
    class CriticalSectionTestTask : public Tasks::Task
    {
    public:
        CriticalSectionTestTask(CS_T& cs, std::chrono::milliseconds targetDuration, std::atomic<std::size_t>& completedCount,
                                std::atomic<bool>& violationDetected, std::atomic<int>& csInUse) noexcept
            : cs_(cs),
              targetDuration_(targetDuration),
              completedCount_(completedCount),
              violationDetected_(violationDetected),
              csInUse_(csInUse)
        {}

        void Run() noexcept override
        {
            const auto startTime = std::chrono::steady_clock::now();

            while ((std::chrono::steady_clock::now() - startTime) < targetDuration_) {
                cs_.Enter();

                int prev = csInUse_.fetch_add(1, std::memory_order_relaxed);
                if (prev != 0) {
                    violationDetected_.store(true, std::memory_order_relaxed);
                }

                std::atomic_signal_fence(std::memory_order_acq_rel);

                csInUse_.fetch_sub(1, std::memory_order_relaxed);
                cs_.Leave();

                GetTaskScheduler()->YieldTask();
            }

            completedCount_.fetch_add(1, std::memory_order_release);
        }

    private:
        CS_T&                     cs_;
        std::chrono::milliseconds targetDuration_;
        std::atomic<std::size_t>& completedCount_;
        std::atomic<bool>&        violationDetected_;
        std::atomic<int>&         csInUse_;
    };

    TEST_CLASS(CriticalSectionTests)
    {
    public:
        TEST_METHOD(TestCriticalSectionConcurrency)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());

            LogDebugMessage(std::format(L"Starting CriticalSection concurrency tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                LogDebugMessage(std::format(L"Testing CriticalSection: {} tasks across {} thread(s) (target execution: {}ms)...",
                                             NumTasks, numThreads, TaskExecutionDuration.count()));

                ConvertThreadToTask();

                SchedulerThreadTracker tracker;
                SchedulerInfo          schedulerInfo
                    = {.numberOfThreads = numThreads, .threadCallback = SchedulerThreadTracker::ThreadCallback, .threadCallbackContext = &tracker};

                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::CriticalSection cs;
                std::atomic<std::size_t>          completedCount{0};
                std::atomic<bool>                 violationDetected{false};
                std::atomic<int>                  csInUse{0};

                std::vector<std::unique_ptr<CriticalSectionTestTask<Synchronization::CriticalSection>>> tasks;
                tasks.reserve(NumTasks);
                for (std::size_t i = 0; i < NumTasks; ++i) {
                    tasks.push_back(std::make_unique<CriticalSectionTestTask<Synchronization::CriticalSection>>(
                        cs, TaskExecutionDuration, completedCount, violationDetected, csInUse));
                }

                Synchronization::TaskGroup taskGroup;
                for (std::size_t i = 0; i < NumTasks; ++i) {
                    taskGroup.AddTask(tasks[i].get());
                    scheduler.AddTask(tasks[i].get(), TaskPriority::Normal);
                }

                taskGroup.Wait();

                const bool allCompleted = (completedCount.load(std::memory_order_acquire) == NumTasks);

                if (!allCompleted) {
                    tracker.SuspendAll();

                    const std::size_t finalCompleted = completedCount.load(std::memory_order_relaxed);
                    const bool        finalViolation = violationDetected.load(std::memory_order_relaxed);
                    const int         finalCsInUse   = csInUse.load(std::memory_order_relaxed);

                    std::wstring errorMsg = std::format(
                        L"Thread count {} test timed out! Completed {}/{} tasks. CS in use: {}, Violation detected: {}",
                        numThreads,
                        finalCompleted,
                        NumTasks,
                        finalCsInUse,
                        finalViolation
                    );

                    tracker.ResumeAll();
                    scheduler.Stop();
                    ConvertTaskToThread();
                    tracker.CloseHandles();

                    Assert::Fail(errorMsg.c_str());
                }

                Assert::IsFalse(violationDetected.load(std::memory_order_relaxed), L"CriticalSection mutual exclusion was violated!");

                scheduler.Stop();
                ConvertTaskToThread();
                tracker.CloseHandles();

                LogDebugMessage(std::format(L"Passed CriticalSection test for {} thread(s).", numThreads));
            }
        }

        TEST_METHOD(TestCriticalSectionSingleSuspensionConcurrency)
        {
            const unsigned int  maxThreads           = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumSingleWaiterTasks = 2;

            LogDebugMessage(std::format(L"Starting CriticalSection_s (single-waiter) concurrency tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerThreadTracker tracker;
                SchedulerInfo          schedulerInfo
                    = {.numberOfThreads = numThreads, .threadCallback = SchedulerThreadTracker::ThreadCallback, .threadCallbackContext = &tracker};

                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::CriticalSection_s cs;
                std::atomic<std::size_t>            completedCount{0};
                std::atomic<bool>                   violationDetected{false};
                std::atomic<int>                    csInUse{0};

                std::vector<std::unique_ptr<CriticalSectionTestTask<Synchronization::CriticalSection_s>>> tasks;
                tasks.reserve(NumSingleWaiterTasks);
                for (std::size_t i = 0; i < NumSingleWaiterTasks; ++i) {
                    tasks.push_back(std::make_unique<CriticalSectionTestTask<Synchronization::CriticalSection_s>>(
                        cs, TaskExecutionDuration, completedCount, violationDetected, csInUse));
                }

                Synchronization::TaskGroup taskGroup;
                for (std::size_t i = 0; i < NumSingleWaiterTasks; ++i) {
                    taskGroup.AddTask(tasks[i].get());
                    scheduler.AddTask(tasks[i].get(), TaskPriority::Normal);
                }

                taskGroup.Wait();

                const bool allCompleted = (completedCount.load(std::memory_order_acquire) == NumSingleWaiterTasks);

                if (!allCompleted) {
                    tracker.SuspendAll();

                    const std::size_t finalCompleted = completedCount.load(std::memory_order_relaxed);
                    const bool        finalViolation = violationDetected.load(std::memory_order_relaxed);
                    const int         finalCsInUse   = csInUse.load(std::memory_order_relaxed);

                    std::wstring errorMsg = std::format(
                        L"CriticalSection_s thread count {} test timed out! Completed {}/{} tasks. CS in use: {}, Violation detected: {}",
                        numThreads,
                        finalCompleted,
                        NumSingleWaiterTasks,
                        finalCsInUse,
                        finalViolation
                    );

                    tracker.ResumeAll();
                    scheduler.Stop();
                    ConvertTaskToThread();
                    tracker.CloseHandles();

                    Assert::Fail(errorMsg.c_str());
                }

                Assert::IsFalse(violationDetected.load(std::memory_order_relaxed), L"CriticalSection_s mutual exclusion was violated!");

                scheduler.Stop();
                ConvertTaskToThread();
                tracker.CloseHandles();
            }
        }

        TEST_METHOD(TestReentrancy)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskRan{false};

                auto lambdaTask = Tasks::Task::Create([&taskRan]() {
                    Synchronization::CriticalSection cs;

                    Assert::IsTrue(cs.TryEnter(), L"First TryEnter should succeed");
                    Assert::IsTrue(cs.TryEnter(), L"Re-entrant TryEnter should succeed");
                    cs.Enter(); // Depth 3

                    // Leave 3 times
                    cs.Leave();
                    cs.Leave();
                    cs.Leave();

                    // The critical section must be fully released after balanced leaves.
                    Assert::IsTrue(cs.TryEnter(), L"CS should be re-acquirable after balanced Leave");
                    cs.Leave();

                    taskRan.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&lambdaTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskRan] { return taskRan.load(std::memory_order_acquire); });

                Assert::IsTrue(finished, L"Re-entrancy test task did not finish.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTryEnter)
        {
            for (DWORD numThreads : GetMultiThreadTestCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::CriticalSection cs;
                std::atomic<bool> holderEntered{false};
                std::atomic<bool> tryEnterResult{true};
                std::atomic<bool> releaseHolder{false};
                std::atomic<bool> contenderDone{false};
                std::atomic<bool> holderDone{false};

                auto holderTask = Tasks::Task::Create([&cs, &holderEntered, &releaseHolder, &holderDone]() {
                    cs.Enter();
                    holderEntered.store(true, std::memory_order_release);

                    while (!releaseHolder.load(std::memory_order_acquire)) {
                        GetTaskScheduler()->YieldTask();
                    }

                    cs.Leave();
                    holderDone.store(true, std::memory_order_release);
                });

                auto contenderTask = Tasks::Task::Create([&cs, &holderEntered, &tryEnterResult, &contenderDone]() {
                    while (!holderEntered.load(std::memory_order_acquire)) {
                        GetTaskScheduler()->YieldTask();
                    }

                    bool acquired = cs.TryEnter();
                    tryEnterResult.store(acquired, std::memory_order_release);
                    if (acquired) {
                        cs.Leave();
                    }

                    contenderDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&holderTask, TaskPriority::Normal);
                scheduler.AddTask(&contenderTask, TaskPriority::Normal);

                const bool contenderFinished = WaitFor([&contenderDone] { return contenderDone.load(std::memory_order_acquire); });
                Assert::IsTrue(contenderFinished, L"Contender task did not finish.");

                Assert::IsFalse(tryEnterResult.load(std::memory_order_acquire), L"TryEnter should return false when CS is held by another task");

                releaseHolder.store(true, std::memory_order_release);

                const bool holderFinished = WaitFor([&holderDone] { return holderDone.load(std::memory_order_acquire); });
                Assert::IsTrue(holderFinished, L"Holder task did not finish.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestRaiiLocks)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskRan{false};

                auto lockTask = Tasks::Task::Create([&taskRan]() {
                    Synchronization::CriticalSection cs;

                    {
                        Synchronization::CriticalSectionLock lock(cs);
                        // Scope held lock
                    }

                    {
                        Synchronization::SpinCriticalSectionLock spinLock(cs, 100);
                        // Scope held spin lock
                    }

                    taskRan.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&lockTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskRan] { return taskRan.load(std::memory_order_acquire); });

                Assert::IsTrue(finished, L"RAII Lock test task did not complete.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestSpinLockContendedFallback)
        {
            for (DWORD numThreads : GetMultiThreadTestCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::CriticalSection cs;
                std::atomic<bool> holderEntered{false};
                std::atomic<bool> releaseHolder{false};
                std::atomic<bool> spinnerAcquired{false};
                std::atomic<bool> spinnerDone{false};
                std::atomic<bool> holderDone{false};

                auto holderTask = Tasks::Task::Create([&cs, &holderEntered, &releaseHolder, &holderDone]() {
                    cs.Enter();
                    holderEntered.store(true, std::memory_order_release);

                    // Hold the lock long enough that the spinner exhausts its spin count
                    // and is forced down the blocking Enter() fallback path.
                    while (!releaseHolder.load(std::memory_order_acquire))
                        GetTaskScheduler()->YieldTask();

                    cs.Leave();
                    holderDone.store(true, std::memory_order_release);
                });

                auto spinnerTask = Tasks::Task::Create([&cs, &holderEntered, &spinnerAcquired, &spinnerDone]() {
                    while (!holderEntered.load(std::memory_order_acquire))
                        GetTaskScheduler()->YieldTask();

                    {
                        // A small spin count against a held lock guarantees the spin loop
                        // is exhausted and the RAII wrapper falls back to Enter().
                        Synchronization::SpinCriticalSectionLock spinLock(cs, 10);
                        spinnerAcquired.store(true, std::memory_order_release);
                    }

                    spinnerDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&holderTask, TaskPriority::Normal);
                scheduler.AddTask(&spinnerTask, TaskPriority::Normal);

                // Let the spinner reach and exhaust its spin loop while the lock is held.
                Assert::IsTrue(WaitFor([&holderEntered] { return holderEntered.load(std::memory_order_acquire); }), L"Holder never entered the CS.");
                SimulateCpuLoad(std::chrono::milliseconds(50));
                Assert::IsFalse(spinnerAcquired.load(std::memory_order_acquire), L"Spinner must not acquire while the lock is held.");

                releaseHolder.store(true, std::memory_order_release);

                Assert::IsTrue(WaitFor([&spinnerDone] { return spinnerDone.load(std::memory_order_acquire); }), L"Spinner did not acquire after release.");
                Assert::IsTrue(WaitFor([&holderDone] { return holderDone.load(std::memory_order_acquire); }), L"Holder did not finish.");
                Assert::IsTrue(spinnerAcquired.load(std::memory_order_acquire), L"Spinner should have acquired the CS after fallback.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestRaiiLocksSingleWaiter)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskRan{false};

                auto lockTask = Tasks::Task::Create([&taskRan]() {
                    Synchronization::CriticalSection_s cs;

                    {
                        Synchronization::CriticalSectionLock lock(cs);
                        // Scope held single-waiter lock
                    }

                    {
                        Synchronization::SpinCriticalSectionLock spinLock(cs, 100);
                        // Scope held single-waiter spin lock
                    }

                    // Lock must be reacquirable after both RAII scopes released it.
                    Assert::IsTrue(cs.TryEnter(), L"CriticalSection_s should be free after RAII locks exit");
                    cs.Leave();

                    taskRan.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&lockTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskRan] { return taskRan.load(std::memory_order_acquire); });

                Assert::IsTrue(finished, L"RAII Lock test task did not complete.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
