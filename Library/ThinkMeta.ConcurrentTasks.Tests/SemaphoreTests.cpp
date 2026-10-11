#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    // Constructed when the test DLL loads, on a plain thread that is no task: a Multiple primitive
    // must not need one for its construction.
    Synchronization::Semaphore globalSemaphore;

    TEST_CLASS(SemaphoreTests)
    {
    public:
        TEST_METHOD(TestGlobalSemaphore)
        {
            ConvertThreadToTask();

            SchedulerInfo   schedulerInfo = {.numberOfThreads = 2};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            std::atomic<int> passed{0};
            ActionTask       waiter1([&passed]() {
                globalSemaphore.Wait();
                passed.fetch_add(1, std::memory_order_release);
            });
            ActionTask       waiter2([&passed]() {
                globalSemaphore.Wait();
                passed.fetch_add(1, std::memory_order_release);
            });
            scheduler.AddTask(&waiter1, TaskPriority::Normal);
            scheduler.AddTask(&waiter2, TaskPriority::Normal);

            globalSemaphore.Release(2);

            const bool finished = WaitFor([&passed] { return passed.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(10));
            Assert::IsTrue(finished, L"tasks waiting on a global Semaphore were not released");

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestSemaphoreConcurrency)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t MaxPermits = 4;
            constexpr std::size_t NumTasks = 16;

            LogDebugMessage(std::format(L"Starting Semaphore concurrency tests (limit: {}) for 1..{} threads.", MaxPermits, maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::Semaphore sem(MaxPermits);
                std::atomic<std::size_t> activeWorkers{0};
                std::atomic<bool> maxPermitViolation{false};
                std::atomic<std::size_t> completedCount{0};

                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(NumTasks);

                for (std::size_t i = 0; i < NumTasks; ++i) {
                    tasks.push_back(std::make_unique<ActionTask>([&sem, &activeWorkers, &maxPermitViolation, &completedCount]() {
                        sem.Wait();

                        std::size_t current = activeWorkers.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (current > MaxPermits) {
                            maxPermitViolation.store(true, std::memory_order_relaxed);
                        }

                        SimulateCpuLoad(std::chrono::milliseconds(20));

                        activeWorkers.fetch_sub(1, std::memory_order_relaxed);
                        sem.Release();

                        completedCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(tasks.back().get(), TaskPriority::Normal);
                }

                const bool finished = WaitFor([&completedCount] { return completedCount.load(std::memory_order_acquire) == NumTasks; }, std::chrono::seconds(10));

                Assert::IsTrue(finished, L"Semaphore concurrency test timed out!");
                Assert::IsFalse(maxPermitViolation.load(std::memory_order_relaxed), L"Semaphore capacity bound was violated!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestSemaphoreSingleSuspensionConcurrency)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumSingleWaiterTasks = 2;

            LogDebugMessage(std::format(L"Starting Semaphore_s (single-waiter) concurrency tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::Semaphore_s sem(1);
                std::atomic<std::size_t> activeWorkers{0};
                std::atomic<bool> violationDetected{false};
                std::atomic<std::size_t> completedCount{0};

                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(NumSingleWaiterTasks);

                for (std::size_t i = 0; i < NumSingleWaiterTasks; ++i) {
                    tasks.push_back(std::make_unique<ActionTask>([&sem, &activeWorkers, &violationDetected, &completedCount]() {
                        sem.Wait();

                        std::size_t current = activeWorkers.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (current > 1) {
                            violationDetected.store(true, std::memory_order_relaxed);
                        }

                        SimulateCpuLoad(std::chrono::milliseconds(20));

                        activeWorkers.fetch_sub(1, std::memory_order_relaxed);
                        sem.Release();

                        completedCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(tasks.back().get(), TaskPriority::Normal);
                }

                const bool finished = WaitFor([&completedCount] { return completedCount.load(std::memory_order_acquire) == NumSingleWaiterTasks; }, std::chrono::seconds(5));

                Assert::IsTrue(finished, L"Semaphore_s single-waiter test timed out!");
                Assert::IsFalse(violationDetected.load(std::memory_order_relaxed), L"Semaphore_s mutual exclusion was violated!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestSemaphoreReleaseCount)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::Semaphore sem(0);

                    // Releasing 3 permits at once
                    sem.Release(3);

                    Assert::IsTrue(sem.TryWait(), L"First TryWait should succeed");
                    Assert::IsTrue(sem.TryWait(), L"Second TryWait should succeed");
                    Assert::IsTrue(sem.TryWait(), L"Third TryWait should succeed");
                    Assert::IsFalse(sem.TryWait(), L"Fourth TryWait should return false");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Semaphore Release(N) test task timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTryWait)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::Semaphore sem(1);

                    Assert::IsTrue(sem.TryWait(), L"TryWait on count 1 should succeed");
                    Assert::IsFalse(sem.TryWait(), L"TryWait on count 0 should return false");

                    sem.Release();
                    Assert::IsTrue(sem.TryWait(), L"TryWait after Release should succeed");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Semaphore TryWait test task timed out!");

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

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::Semaphore sem(2);

                    {
                        Synchronization::SemaphoreLock lock(sem);
                        // Scope held permit 1
                    } // Released permit 1

                    {
                        Synchronization::SpinSemaphoreLock spinLock(sem, 100);
                        // Scope held permit 2
                    } // Released permit 2

                    Assert::IsTrue(sem.TryWait(), L"Semaphore count should be restored after RAII locks exit");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Semaphore RAII locks test task timed out!");

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

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::Semaphore_s sem(1);

                    {
                        Synchronization::SemaphoreLock lock(sem);
                        // Scope held the single permit
                    } // Released permit

                    {
                        Synchronization::SpinSemaphoreLock spinLock(sem, 100);
                        // Scope held the single permit
                    } // Released permit

                    Assert::IsTrue(sem.TryWait(), L"Semaphore_s count should be restored after RAII locks exit");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Semaphore_s RAII locks test task timed out!");

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

                Synchronization::Semaphore sem(1);
                std::atomic<bool> holderAcquired{false};
                std::atomic<bool> releaseHolder{false};
                std::atomic<bool> spinnerAcquired{false};
                std::atomic<bool> spinnerDone{false};
                std::atomic<bool> holderDone{false};

                auto holderTask = Tasks::Task::Create([&sem, &holderAcquired, &releaseHolder, &holderDone]() {
                    sem.Wait();
                    holderAcquired.store(true, std::memory_order_release);

                    // Hold the only permit until released so the spinner must fall back to Wait().
                    while (!releaseHolder.load(std::memory_order_acquire))
                        GetTaskScheduler()->YieldTask();

                    sem.Release();
                    holderDone.store(true, std::memory_order_release);
                });

                auto spinnerTask = Tasks::Task::Create([&sem, &holderAcquired, &spinnerAcquired, &spinnerDone]() {
                    while (!holderAcquired.load(std::memory_order_acquire))
                        GetTaskScheduler()->YieldTask();

                    {
                        // A small spin count against an exhausted semaphore guarantees the
                        // spin loop is exhausted and the RAII wrapper falls back to Wait().
                        Synchronization::SpinSemaphoreLock spinLock(sem, 10);
                        spinnerAcquired.store(true, std::memory_order_release);
                    }

                    spinnerDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&holderTask, TaskPriority::Normal);
                scheduler.AddTask(&spinnerTask, TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&holderAcquired] { return holderAcquired.load(std::memory_order_acquire); }), L"Holder never acquired the permit.");
                SimulateCpuLoad(std::chrono::milliseconds(50));
                Assert::IsFalse(spinnerAcquired.load(std::memory_order_acquire), L"Spinner must not acquire while the only permit is held.");

                releaseHolder.store(true, std::memory_order_release);

                Assert::IsTrue(WaitFor([&spinnerDone] { return spinnerDone.load(std::memory_order_acquire); }), L"Spinner did not acquire after release.");
                Assert::IsTrue(WaitFor([&holderDone] { return holderDone.load(std::memory_order_acquire); }), L"Holder did not finish.");
                Assert::IsTrue(spinnerAcquired.load(std::memory_order_acquire), L"Spinner should have acquired the permit after fallback.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
