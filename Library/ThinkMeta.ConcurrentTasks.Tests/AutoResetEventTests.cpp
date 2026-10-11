#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(AutoResetEventTests)
    {
    public:
        TEST_METHOD(TestAutoResetEventConcurrency)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumConsumers = 16;

            LogDebugMessage(std::format(L"Starting AutoResetEvent concurrency tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::AutoResetEvent autoEvent(false);
                std::atomic<std::size_t> consumedCount{0};

                std::vector<std::unique_ptr<ActionTask>> consumerTasks;
                consumerTasks.reserve(NumConsumers);

                for (std::size_t i = 0; i < NumConsumers; ++i) {
                    consumerTasks.push_back(std::make_unique<ActionTask>([&autoEvent, &consumedCount]() {
                        autoEvent.Wait();
                        consumedCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(consumerTasks.back().get(), TaskPriority::Normal);
                }

                auto producerTask = Tasks::Task::Create([&autoEvent, &consumedCount]() {
                    for (std::size_t i = 0; i < NumConsumers; ++i) {
                        autoEvent.Set();
                        // Wait until this signal is consumed before sending the next one
                        const std::size_t expected = i + 1;
                        YieldWaitFor([&consumedCount, expected] { return consumedCount.load(std::memory_order_acquire) >= expected; });
                    }
                });

                scheduler.AddTask(&producerTask, TaskPriority::Normal);

                const bool finished = WaitFor([&consumedCount] { return consumedCount.load(std::memory_order_acquire) == NumConsumers; }, std::chrono::seconds(10));

                Assert::IsTrue(finished, L"AutoResetEvent producer-consumer test timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestAutoResetEventSingleWaitConcurrency)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t Iterations = 20;

            LogDebugMessage(std::format(L"Starting AutoResetEvent_s (single-waiter) tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::AutoResetEvent_s autoEvent(false);
                std::atomic<std::size_t> pingPongCount{0};

                auto consumerTask = Tasks::Task::Create([&autoEvent, &pingPongCount]() {
                    for (std::size_t i = 0; i < Iterations; ++i) {
                        autoEvent.Wait();
                        pingPongCount.fetch_add(1, std::memory_order_release);
                    }
                });

                auto producerTask = Tasks::Task::Create([&autoEvent, &pingPongCount]() {
                    for (std::size_t i = 0; i < Iterations; ++i) {
                        autoEvent.Set();
                        const std::size_t expected = i + 1;
                        YieldWaitFor([&pingPongCount, expected] { return pingPongCount.load(std::memory_order_acquire) >= expected; });
                    }
                });

                scheduler.AddTask(&consumerTask, TaskPriority::Normal);
                scheduler.AddTask(&producerTask, TaskPriority::Normal);

                const bool finished = WaitFor([&pingPongCount] { return pingPongCount.load(std::memory_order_acquire) == Iterations; }, std::chrono::seconds(10));

                Assert::IsTrue(finished, L"AutoResetEvent_s single-waiter test timed out!");

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
                    Synchronization::AutoResetEvent initialSetEvent(true);

                    // Should succeed immediately because it started in signaled state
                    Assert::IsTrue(initialSetEvent.TryWait(), L"TryWait on initially set event should succeed");
                    // Should fail now because AutoResetEvent auto-resets upon consumption
                    Assert::IsFalse(initialSetEvent.TryWait(), L"TryWait after consumption should fail");

                    // Test manual Reset()
                    initialSetEvent.Set();
                    initialSetEvent.Reset();
                    Assert::IsFalse(initialSetEvent.TryWait(), L"TryWait after Reset should return false");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Initial state and Reset test task timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestSetBeforeWait)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::AutoResetEvent autoEvent(false);

                    autoEvent.Set();
                    autoEvent.Wait(); // Should unblock immediately without suspending

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Set before Wait test task timed out!");

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
                    Synchronization::AutoResetEvent autoEvent(false);

                    Assert::IsFalse(autoEvent.TryWait(), L"TryWait on unsignaled event must return false");

                    autoEvent.Set();
                    Assert::IsTrue(autoEvent.TryWait(), L"TryWait on set event must return true");
                    Assert::IsFalse(autoEvent.TryWait(), L"Second TryWait must return false due to auto-reset");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"TryWait test task timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestWakeExactlyOne)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumWaiters = 4;

            LogDebugMessage(std::format(L"Starting AutoResetEvent wake-exactly-one tests for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::AutoResetEvent autoEvent(false);
                std::atomic<std::size_t> wokenCount{0};
                std::atomic<std::size_t> parkedCount{0};

                std::vector<std::unique_ptr<ActionTask>> waiterTasks;
                waiterTasks.reserve(NumWaiters);

                for (std::size_t i = 0; i < NumWaiters; ++i) {
                    waiterTasks.push_back(std::make_unique<ActionTask>([&autoEvent, &wokenCount, &parkedCount]() {
                        parkedCount.fetch_add(1, std::memory_order_release);
                        autoEvent.Wait();
                        wokenCount.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(waiterTasks.back().get(), TaskPriority::Normal);
                }

                // Ensure every waiter has reached its Wait() before signaling.
                Assert::IsTrue(WaitFor([&parkedCount] { return parkedCount.load(std::memory_order_acquire) == NumWaiters; }, std::chrono::seconds(10)), L"Not all waiters parked before Set().");

                // A single Set() must wake exactly one waiter.
                autoEvent.Set();

                Assert::IsTrue(WaitFor([&wokenCount] { return wokenCount.load(std::memory_order_acquire) >= 1; }, std::chrono::seconds(10)), L"No waiter woke after Set().");

                // Give any erroneously-woken waiters time to increment before asserting.
                SimulateCpuLoad(std::chrono::milliseconds(50));
                Assert::AreEqual(static_cast<std::size_t>(1), wokenCount.load(std::memory_order_acquire), L"Exactly one waiter must wake per Set().");

                // Release the remaining waiters so the scheduler can drain cleanly.
                for (std::size_t i = 1; i < NumWaiters; ++i) {
                    const std::size_t expected = i + 1;
                    autoEvent.Set();
                    WaitFor([&wokenCount, expected] { return wokenCount.load(std::memory_order_acquire) >= expected; }, std::chrono::seconds(10));
                }

                Assert::IsTrue(WaitFor([&wokenCount] { return wokenCount.load(std::memory_order_acquire) == NumWaiters; }, std::chrono::seconds(10)), L"Not all waiters woke after subsequent Set() calls.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        // A Set() that resumes a suspended waiter hands the signal to that waiter: the event
        // must be reset afterwards, or the waiter's next Wait() passes without a Set().
        template <class Event>
        static void CheckSetToSuspendedWaiterConsumesSignal(const wchar_t* name)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Event             event(false);
                std::atomic<bool> parked{false};
                std::atomic<bool> woken{false};

                auto waiterTask = Tasks::Task::Create([&event, &parked, &woken]() {
                    parked.store(true, std::memory_order_release);
                    event.Wait();
                    woken.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&waiterTask, TaskPriority::Normal);
                Assert::IsTrue(WaitFor([&parked] { return parked.load(std::memory_order_acquire); }, std::chrono::seconds(10)), name);

                // let the waiter suspend
                Sleep(20);
                event.Set();

                Assert::IsTrue(WaitFor([&woken] { return woken.load(std::memory_order_acquire); }, std::chrono::seconds(10)), name);
                Assert::IsFalse(event.TryWait(), name);

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        // Two tasks hand a token back and forth; every Wait() needs its own Set(). A Set() that
        // leaves the signal behind lets one side run ahead and the other wait forever.
        template <class Event>
        static void CheckPingPong(const wchar_t* name)
        {
            constexpr std::size_t RoundTrips = 20000;

            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Event                    ping(false);
                Event                    pong(false);
                std::atomic<std::size_t> done{0};

                auto first = Tasks::Task::Create([&ping, &pong, &done]() {
                    for (std::size_t i = 0; i < RoundTrips; ++i) {
                        ping.Set();
                        pong.Wait();
                    }
                    done.fetch_add(1, std::memory_order_release);
                });
                auto second = Tasks::Task::Create([&ping, &pong, &done]() {
                    for (std::size_t i = 0; i < RoundTrips; ++i) {
                        ping.Wait();
                        pong.Set();
                    }
                    done.fetch_add(1, std::memory_order_release);
                });

                scheduler.AddTask(&second, TaskPriority::Normal);
                scheduler.AddTask(&first, TaskPriority::Normal);

                const bool finished = WaitFor([&done] { return done.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(20));
                Assert::IsTrue(finished, name);
                Assert::IsFalse(ping.TryWait(), name);
                Assert::IsFalse(pong.TryWait(), name);

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        /// <summary>
        /// Many ping-pong pairs at once on twice as many scheduler threads as processors: a scheduler
        /// thread preempted in OnSuspended gets far more chances to look at the event late, after its
        /// task was resumed and waits again (a late look that takes the next round's signal leaves
        /// both sides waiting).
        /// </summary>
        template <typename Event>
        static void CheckPingPongPairs(const wchar_t* name)
        {
            constexpr std::size_t Pairs      = 32;
            constexpr std::size_t RoundTrips = 20000;

            ConvertThreadToTask();
            const DWORD     numThreads    = 2 * std::max(1u, std::thread::hardware_concurrency());
            SchedulerInfo   schedulerInfo = {.numberOfThreads = numThreads};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            struct Pair {
                Event ping{false};
                Event pong{false};
            };
            const auto               pairs = std::make_unique<Pair[]>(Pairs);
            std::atomic<std::size_t> done{0};

            std::vector<std::unique_ptr<ActionTask>> tasks;
            for (std::size_t p = 0; p < Pairs; ++p) {
                Pair& pair = pairs[p];
                tasks.push_back(std::make_unique<ActionTask>([&pair, &done]() {
                    for (std::size_t i = 0; i < RoundTrips; ++i) {
                        pair.ping.Set();
                        pair.pong.Wait();
                    }
                    done.fetch_add(1, std::memory_order_release);
                }));
                tasks.push_back(std::make_unique<ActionTask>([&pair, &done]() {
                    for (std::size_t i = 0; i < RoundTrips; ++i) {
                        pair.ping.Wait();
                        pair.pong.Set();
                    }
                    done.fetch_add(1, std::memory_order_release);
                }));
            }
            for (auto& task : tasks)
                scheduler.AddTask(task.get(), TaskPriority::Normal);

            const bool finished = WaitFor([&done] { return done.load(std::memory_order_acquire) == 2 * Pairs; }, std::chrono::seconds(60));
            Assert::IsTrue(finished, name);
            for (std::size_t p = 0; p < Pairs; ++p) {
                Assert::IsFalse(pairs[p].ping.TryWait(), name);
                Assert::IsFalse(pairs[p].pong.TryWait(), name);
            }

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestPingPongManyPairsSingleWaiter)
        {
            CheckPingPongPairs<Synchronization::AutoResetEvent_s>(L"AutoResetEvent_s: a ping-pong pair stopped");
        }

        TEST_METHOD(TestSetToSuspendedWaiterConsumesSignal)
        {
            CheckSetToSuspendedWaiterConsumesSignal<Synchronization::AutoResetEvent>(L"AutoResetEvent: Set() to a suspended waiter left the event set");
            CheckSetToSuspendedWaiterConsumesSignal<Synchronization::AutoResetEvent_s>(L"AutoResetEvent_s: Set() to a suspended waiter left the event set");
        }

        TEST_METHOD(TestPingPong)
        {
            CheckPingPong<Synchronization::AutoResetEvent>(L"AutoResetEvent ping-pong failed");
        }

        TEST_METHOD(TestPingPongSingleWaiter)
        {
            CheckPingPong<Synchronization::AutoResetEvent_s>(L"AutoResetEvent_s ping-pong failed");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
