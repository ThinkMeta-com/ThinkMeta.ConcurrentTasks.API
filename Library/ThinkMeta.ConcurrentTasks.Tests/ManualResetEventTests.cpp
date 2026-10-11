#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(ManualResetEventTests)
    {
    public:
        TEST_METHOD(TestManualResetEventBroadcast)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumWaiters = 16;

            LogDebugMessage(std::format(L"Starting ManualResetEvent broadcast tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::ManualResetEvent manualEvent(false);
                std::atomic<std::size_t> wokeCount{0};

                std::vector<std::unique_ptr<ActionTask>> waiterTasks;
                waiterTasks.reserve(NumWaiters);

                for (std::size_t i = 0; i < NumWaiters; ++i) {
                    waiterTasks.push_back(std::make_unique<ActionTask>([&manualEvent, &wokeCount]() {
                        manualEvent.Wait();
                        wokeCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(waiterTasks.back().get(), TaskPriority::Normal);
                }

                // Give tasks time to queue and suspend on manualEvent
                SimulateCpuLoad(std::chrono::milliseconds(50));

                // Broadcast wake-up to all waiting tasks
                manualEvent.Set();

                const bool finished = WaitFor([&wokeCount] { return wokeCount.load(std::memory_order_acquire) == NumWaiters; }, std::chrono::seconds(5));

                Assert::IsTrue(finished, L"ManualResetEvent broadcast test timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestManualResetEventSingleWaiter)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());

            LogDebugMessage(std::format(L"Starting ManualResetEvent_s (single-waiter) tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::ManualResetEvent_s manualEvent(false);
                std::atomic<bool> wokeUp{false};

                auto waiterTask = Tasks::Task::Create([&manualEvent, &wokeUp]() {
                    manualEvent.Wait();
                    wokeUp.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&waiterTask, TaskPriority::Normal);
                SimulateCpuLoad(std::chrono::milliseconds(50));

                manualEvent.Set();

                const bool finished = WaitFor([&wokeUp] { return wokeUp.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"ManualResetEvent_s single-waiter test timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestInitialStateAndReset)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::ManualResetEvent setEvent(true);

                    // Multiple TryWait calls should succeed without auto-resetting
                    Assert::IsTrue(setEvent.TryWait(), L"First TryWait on set event should be true");
                    Assert::IsTrue(setEvent.TryWait(), L"Second TryWait on set event should be true");

                    // Manual Reset() clears the signal
                    setEvent.Reset();
                    Assert::IsFalse(setEvent.TryWait(), L"TryWait after Reset() must return false");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"ManualResetEvent initial state and Reset test timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestReuseAfterReset)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::ManualResetEvent manualEvent(false);

                    // Round 1
                    manualEvent.Set();
                    manualEvent.Wait(); // Immediate return
                    manualEvent.Reset();
                    Assert::IsFalse(manualEvent.TryWait());

                    // Round 2
                    manualEvent.Set();
                    manualEvent.Wait(); // Immediate return
                    Assert::IsTrue(manualEvent.TryWait());

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"ManualResetEvent reuse after reset test timed out!");

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
                    Synchronization::ManualResetEvent manualEvent(false);

                    Assert::IsFalse(manualEvent.TryWait(), L"TryWait on unsignaled event must return false");

                    manualEvent.Set();
                    Assert::IsTrue(manualEvent.TryWait(), L"TryWait on set event must return true");
                    Assert::IsTrue(manualEvent.TryWait(), L"ManualResetEvent does not auto-reset");

                    manualEvent.Reset();
                    Assert::IsFalse(manualEvent.TryWait(), L"TryWait after Reset must return false");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"ManualResetEvent TryWait test timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestParkedWaiterReleasedBeforeShutdown)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumWaiters = 8;

            LogDebugMessage(std::format(L"Starting ManualResetEvent parked-waiter shutdown tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::ManualResetEvent manualEvent(false);
                std::atomic<std::size_t> parkedCount{0};
                std::atomic<std::size_t> wokeCount{0};

                std::vector<std::unique_ptr<ActionTask>> waiterTasks;
                waiterTasks.reserve(NumWaiters);

                for (std::size_t i = 0; i < NumWaiters; ++i) {
                    waiterTasks.push_back(std::make_unique<ActionTask>([&manualEvent, &parkedCount, &wokeCount]() {
                        parkedCount.fetch_add(1, std::memory_order_release);
                        manualEvent.Wait();
                        wokeCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(waiterTasks.back().get(), TaskPriority::Normal);
                }

                // Ensure every waiter is parked on the event before releasing them.
                Assert::IsTrue(WaitFor([&parkedCount] { return parkedCount.load(std::memory_order_acquire) == NumWaiters; }, std::chrono::seconds(10)), L"Not all waiters parked before Set().");

                // Release all parked waiters, then ensure they all drain before shutdown.
                manualEvent.Set();

                Assert::IsTrue(WaitFor([&wokeCount] { return wokeCount.load(std::memory_order_acquire) == NumWaiters; }, std::chrono::seconds(10)), L"Not all parked waiters were released before shutdown.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
