#include "pch.h"
#include "TestHelpers.h"
#include <set>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(SchedulerStarvationTests)
    {
    public:
        TEST_METHOD(TestPriorityStarvationAndFairness)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestPriorityStarvationAndFairness for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing priority starvation with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                constexpr std::size_t NumTasksPerPriority = 100;
                std::vector<std::size_t> executionOrder;
                std::mutex orderMutex;

                std::vector<std::unique_ptr<ActionTask>> normalTasks;
                normalTasks.reserve(NumTasksPerPriority);

                std::vector<std::unique_ptr<ActionTask>> highTasks;
                highTasks.reserve(NumTasksPerPriority);

                for (std::size_t i = 0; i < NumTasksPerPriority; ++i) {
                    normalTasks.push_back(std::make_unique<ActionTask>([i, &executionOrder, &orderMutex]() {
                        SimulateCpuLoad(std::chrono::microseconds(500));
                        for (std::size_t step = 0; step < 20; ++step) {
                            GetTaskScheduler()->YieldTask();
                        }
                        std::lock_guard<std::mutex> lock(orderMutex);
                        executionOrder.push_back(1000 + i); // Normal priority markers: 1000+
                    }));

                    highTasks.push_back(std::make_unique<ActionTask>([i, &executionOrder, &orderMutex]() {
                        SimulateCpuLoad(std::chrono::microseconds(500));
                        for (std::size_t step = 0; step < 20; ++step) {
                            GetTaskScheduler()->YieldTask();
                        }
                        std::lock_guard<std::mutex> lock(orderMutex);
                        executionOrder.push_back(i); // High priority markers: 0+
                    }));
                }

                // Enqueue normal priority tasks first, then high priority tasks
                for (std::size_t i = 0; i < NumTasksPerPriority; ++i) {
                    scheduler.AddTask(normalTasks[i].get(), TaskPriority::Normal);
                }
                for (std::size_t i = 0; i < NumTasksPerPriority; ++i) {
                    scheduler.AddTask(highTasks[i].get(), TaskPriority::High);
                }

                const bool finished = WaitFor([&executionOrder, &orderMutex]() {
                    std::lock_guard<std::mutex> lock(orderMutex);
                    return executionOrder.size() == (NumTasksPerPriority * 2);
                }, std::chrono::seconds(20));

                Assert::IsTrue(finished, L"All priority tasks should complete without timing out");

                {
                    std::lock_guard<std::mutex> lock(orderMutex);
                    Assert::AreEqual(NumTasksPerPriority * 2, executionOrder.size(), L"Execution order count must match total tasks");

                    if (numThreads == 1) {
                        // On 1 worker thread, high priority tasks must execute strictly ahead of normal priority tasks
                        for (std::size_t i = 0; i < NumTasksPerPriority; ++i) {
                            Assert::IsTrue(executionOrder[i] < 1000, L"High priority task should execute before normal priority task");
                        }
                    }

                    // Verify normal priority tasks were NOT starved and all completed
                    std::size_t normalCompletedCount = 0;
                    for (std::size_t val : executionOrder) {
                        if (val >= 1000) {
                            ++normalCompletedCount;
                        }
                    }
                    Assert::AreEqual(NumTasksPerPriority, normalCompletedCount, L"All normal priority tasks must complete without starvation");
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestCooperativeYieldingInterleaving)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestCooperativeYieldingInterleaving for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing cooperative yielding with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                constexpr std::size_t NumConcurrentTasks = 32;
                constexpr std::size_t IterationsPerTask  = 50;

                std::atomic<std::size_t> progress[NumConcurrentTasks] = {};
                std::atomic<std::size_t> completedTasks{0};
                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(NumConcurrentTasks);

                for (std::size_t t = 0; t < NumConcurrentTasks; ++t) {
                    tasks.push_back(std::make_unique<ActionTask>([t, &progress, &completedTasks]() {
                        for (std::size_t step = 0; step < IterationsPerTask; ++step) {
                            progress[t].fetch_add(1, std::memory_order_release);
                            SimulateCpuLoad(std::chrono::microseconds(200));
                            GetTaskScheduler()->YieldTask();
                        }
                        completedTasks.fetch_add(1, std::memory_order_release);
                    }));
                }

                for (std::size_t t = 0; t < NumConcurrentTasks; ++t) {
                    scheduler.AddTask(tasks[t].get(), TaskPriority::Normal);
                }

                const bool finished = WaitFor([&completedTasks]() {
                    return completedTasks.load(std::memory_order_acquire) == NumConcurrentTasks;
                }, std::chrono::seconds(20));

                Assert::IsTrue(finished, L"All cooperative yielding tasks must finish without deadlock");

                for (std::size_t t = 0; t < NumConcurrentTasks; ++t) {
                    Assert::AreEqual(IterationsPerTask, progress[t].load(std::memory_order_acquire), L"Each task should have completed all iterations");
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestYieldFlagsHigherPriority)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestYieldFlagsHigherPriority for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing YieldFlags::HigherPriority with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> highTaskExecuted{false};
                std::atomic<bool> normalTaskFinished{false};
                std::atomic<bool> highExecutedBeforeNormalFinished{false};

                auto highTask = std::make_unique<ActionTask>([&highTaskExecuted]() {
                    highTaskExecuted.store(true, std::memory_order_release);
                    SimulateCpuLoad(std::chrono::microseconds(500));
                    for (std::size_t step = 0; step < 10; ++step) {
                        GetTaskScheduler()->YieldTask();
                    }
                });

                ActionTask* highTaskPtr = highTask.get();

                auto normalTask = std::make_unique<ActionTask>([highTaskPtr, &highTaskExecuted, &normalTaskFinished, &highExecutedBeforeNormalFinished]() {
                    GetTaskScheduler()->AddTask(highTaskPtr, TaskPriority::High);
                    GetTaskScheduler()->YieldTask(YieldFlags::HigherPriority);

                    if (highTaskExecuted.load(std::memory_order_acquire)) {
                        highExecutedBeforeNormalFinished.store(true, std::memory_order_release);
                    }

                    SimulateCpuLoad(std::chrono::microseconds(500));
                    for (std::size_t step = 0; step < 10; ++step) {
                        GetTaskScheduler()->YieldTask();
                    }

                    normalTaskFinished.store(true, std::memory_order_release);
                });

                scheduler.AddTask(normalTask.get(), TaskPriority::Normal);

                const bool finished = WaitFor([&normalTaskFinished]() {
                    return normalTaskFinished.load(std::memory_order_acquire);
                }, std::chrono::seconds(10));

                Assert::IsTrue(finished, L"Normal priority task should finish");
                Assert::IsTrue(highTaskExecuted.load(std::memory_order_acquire), L"High priority task must have executed");
                if (numThreads == 1) {
                    Assert::IsTrue(highExecutedBeforeNormalFinished.load(std::memory_order_acquire), L"YieldFlags::HigherPriority must immediately yield to high priority task on single-threaded scheduler");
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestWorkerThreadActivationAndNoIdleStarvation)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestWorkerThreadActivationAndNoIdleStarvation for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing worker thread activation with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                constexpr std::size_t TotalTasks = 128;
                std::set<std::thread::id> executingThreads;
                std::mutex threadsMutex;
                std::atomic<std::size_t> completedCount{0};

                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(TotalTasks);

                for (std::size_t i = 0; i < TotalTasks; ++i) {
                    tasks.push_back(std::make_unique<ActionTask>([&executingThreads, &threadsMutex, &completedCount]() {
                        {
                            std::lock_guard<std::mutex> lock(threadsMutex);
                            executingThreads.insert(std::this_thread::get_id());
                        }
                        SimulateCpuLoad(std::chrono::milliseconds(5));
                        completedCount.fetch_add(1, std::memory_order_release);
                    }));
                }

                for (std::size_t i = 0; i < TotalTasks; ++i) {
                    scheduler.AddTask(tasks[i].get(), TaskPriority::Normal);
                }

                const bool finished = WaitFor([&completedCount]() {
                    return completedCount.load(std::memory_order_acquire) == TotalTasks;
                }, std::chrono::seconds(15));

                Assert::IsTrue(finished, L"All tasks must complete");

                {
                    std::lock_guard<std::mutex> lock(threadsMutex);
                    Assert::AreEqual(static_cast<std::size_t>(numThreads), executingThreads.size(), L"All configured worker threads must be activated (no idle thread starvation)");
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestCyclicLoadThreadSleepWake)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestCyclicLoadThreadSleepWake for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing cyclic burst load with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                constexpr std::size_t Cycles = 5;
                const std::size_t BurstTasksCount = std::max<std::size_t>(32, static_cast<std::size_t>(numThreads) * 8);

                for (std::size_t cycle = 0; cycle < Cycles; ++cycle) {
                    std::atomic<std::size_t> cycleCompleted{0};
                    std::atomic<std::size_t> activeTasks{0};
                    std::vector<std::unique_ptr<ActionTask>> burstTasks;
                    burstTasks.reserve(BurstTasksCount);

                    for (std::size_t i = 0; i < BurstTasksCount; ++i) {
                        burstTasks.push_back(std::make_unique<ActionTask>([&cycleCompleted, &activeTasks]() {
                            activeTasks.fetch_add(1, std::memory_order_release);
                            SimulateCpuLoad(std::chrono::milliseconds(2));
                            for (std::size_t step = 0; step < 10; ++step) {
                                GetTaskScheduler()->YieldTask();
                            }
                            activeTasks.fetch_sub(1, std::memory_order_release);
                            cycleCompleted.fetch_add(1, std::memory_order_release);
                        }));
                    }

                    // Enqueue burst tasks (task count > worker threads)
                    for (std::size_t i = 0; i < BurstTasksCount; ++i) {
                        scheduler.AddTask(burstTasks[i].get(), TaskPriority::Normal);
                    }

                    // Wait for all burst tasks in cycle to finish
                    const bool cycleFinished = WaitFor([&cycleCompleted, BurstTasksCount]() {
                        return cycleCompleted.load(std::memory_order_acquire) == BurstTasksCount;
                    }, std::chrono::seconds(15));

                    Assert::IsTrue(cycleFinished, L"Burst cycle tasks must complete");
                    Assert::AreEqual(static_cast<std::size_t>(0), activeTasks.load(std::memory_order_acquire), L"Active tasks must drain to 0");

                    // Pause briefly to let worker threads transition to idle sleeping state on WaitOnAddress
                    SimulateCpuLoad(std::chrono::milliseconds(50));
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestVariableActiveThreadSleepResume)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestVariableActiveThreadSleepResume for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing variable active thread sleep and resume with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                constexpr std::size_t Cycles = 3;
                const std::size_t N = static_cast<std::size_t>(numThreads);

                for (std::size_t cycle = 0; cycle < Cycles; ++cycle) {
                    std::atomic<std::size_t> activeTaskCount{0};
                    std::set<std::thread::id> activeThreadIds;
                    std::mutex threadIdMutex;

                    std::vector<std::unique_ptr<Synchronization::ManualResetEvent_s>> events;
                    events.reserve(N);
                    for (std::size_t i = 0; i < N; ++i) {
                        events.push_back(std::make_unique<Synchronization::ManualResetEvent_s>(false));
                    }

                    std::atomic<std::size_t> tasksStarted{0};
                    std::atomic<std::size_t> tasksFinished{0};
                    std::vector<std::unique_ptr<ActionTask>> workerTasks;
                    workerTasks.reserve(N);

                    // Build Tasks 1..N-1 (worker tasks that suspend one-by-one, reducing active threads)
                    for (std::size_t i = 1; i < N; ++i) {
                        workerTasks.push_back(std::make_unique<ActionTask>([i, &events, &activeTaskCount, &tasksStarted, &tasksFinished, &activeThreadIds, &threadIdMutex]() {
                            {
                                std::lock_guard<std::mutex> lock(threadIdMutex);
                                activeThreadIds.insert(std::this_thread::get_id());
                            }
                            tasksStarted.fetch_add(1, std::memory_order_release);
                            activeTaskCount.fetch_add(1, std::memory_order_release);

                            // Suspend on event[i] (forcing worker thread running task i to sleep if no other tasks exist)
                            events[i]->Wait();

                            // Resumed by orchestrator task
                            SimulateCpuLoad(std::chrono::microseconds(200));

                            activeTaskCount.fetch_sub(1, std::memory_order_release);
                            tasksFinished.fetch_add(1, std::memory_order_release);
                        }));
                    }

                    // Build Orchestrator Task 0
                    auto orchestratorTask = std::make_unique<ActionTask>([N, &events, &activeTaskCount, &tasksStarted, &tasksFinished, &activeThreadIds, &threadIdMutex]() {
                        {
                            std::lock_guard<std::mutex> lock(threadIdMutex);
                            activeThreadIds.insert(std::this_thread::get_id());
                        }
                        tasksStarted.fetch_add(1, std::memory_order_release);
                        activeTaskCount.fetch_add(1, std::memory_order_release);

                        // Wait until all N tasks have started running
                        while (tasksStarted.load(std::memory_order_acquire) < N) {
                            GetTaskScheduler()->YieldTask();
                        }

                        // Sequentially resume suspended tasks 1..N-1 one by one from this active task
                        for (std::size_t i = 1; i < N; ++i) {
                            SimulateCpuLoad(std::chrono::milliseconds(5));
                            events[i]->Set();
                            GetTaskScheduler()->YieldTask();
                        }

                        activeTaskCount.fetch_sub(1, std::memory_order_release);
                        tasksFinished.fetch_add(1, std::memory_order_release);
                    });

                    // Enqueue all N tasks to saturate worker threads
                    scheduler.AddTask(orchestratorTask.get(), TaskPriority::Normal);
                    for (std::size_t i = 1; i < N; ++i) {
                        scheduler.AddTask(workerTasks[i - 1].get(), TaskPriority::Normal);
                    }

                    const bool finished = WaitFor([&tasksFinished, N]() {
                        return tasksFinished.load(std::memory_order_acquire) == N;
                    }, std::chrono::seconds(15));

                    Assert::IsTrue(finished, L"Variable active thread sleep/resume cycle timed out!");
                    Assert::AreEqual(N, tasksFinished.load(std::memory_order_acquire), L"All N tasks must complete in variable sleep/resume test");

                    if (N > 1) {
                        std::lock_guard<std::mutex> lock(threadIdMutex);
                        Assert::IsTrue(activeThreadIds.size() > 0 && activeThreadIds.size() <= N, L"Worker threads participating in execution must be non-empty and <= configured thread count");
                    }
                }

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestDynamicTaskSpawningStarvation)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestDynamicTaskSpawningStarvation for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing dynamic task spawning with {} thread(s)", numThreads));
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::TaskGroup taskGroup;
                std::atomic<std::size_t> totalExecuted{0};

                std::vector<std::unique_ptr<ActionTask>> childTasks;
                std::mutex childMutex;

                // Spawns a 4-level tree: 1 root -> 8 children -> 64 grandchildren -> 256 great-grandchildren (329 tasks)
                auto rootTask = std::make_unique<ActionTask>([&taskGroup, &totalExecuted, &childTasks, &childMutex]() {
                    SimulateCpuLoad(std::chrono::microseconds(100));
                    for (std::size_t step = 0; step < 10; ++step) {
                        GetTaskScheduler()->YieldTask();
                    }
                    totalExecuted.fetch_add(1, std::memory_order_release);

                    for (std::size_t i = 0; i < 8; ++i) {
                        auto child = std::make_unique<ActionTask>([&taskGroup, &totalExecuted, &childTasks, &childMutex]() {
                            SimulateCpuLoad(std::chrono::microseconds(100));
                            for (std::size_t step = 0; step < 10; ++step) {
                                GetTaskScheduler()->YieldTask();
                            }
                            totalExecuted.fetch_add(1, std::memory_order_release);

                            for (std::size_t j = 0; j < 8; ++j) {
                                auto grandChild = std::make_unique<ActionTask>([&taskGroup, &totalExecuted, &childTasks, &childMutex]() {
                                    SimulateCpuLoad(std::chrono::microseconds(100));
                                    for (std::size_t step = 0; step < 10; ++step) {
                                        GetTaskScheduler()->YieldTask();
                                    }
                                    totalExecuted.fetch_add(1, std::memory_order_release);

                                    for (std::size_t k = 0; k < 4; ++k) {
                                        auto greatGrandChild = std::make_unique<ActionTask>([&totalExecuted]() {
                                            SimulateCpuLoad(std::chrono::microseconds(100));
                                            for (std::size_t step = 0; step < 10; ++step) {
                                                GetTaskScheduler()->YieldTask();
                                            }
                                            totalExecuted.fetch_add(1, std::memory_order_release);
                                        });

                                        ActionTask* ggcPtr = greatGrandChild.get();
                                        {
                                            std::lock_guard<std::mutex> lock(childMutex);
                                            childTasks.push_back(std::move(greatGrandChild));
                                        }
                                        taskGroup.AddTask(ggcPtr);
                                        GetTaskScheduler()->AddTask(ggcPtr, TaskPriority::Normal);
                                    }
                                });

                                ActionTask* gcPtr = grandChild.get();
                                {
                                    std::lock_guard<std::mutex> lock(childMutex);
                                    childTasks.push_back(std::move(grandChild));
                                }
                                taskGroup.AddTask(gcPtr);
                                GetTaskScheduler()->AddTask(gcPtr, TaskPriority::Normal);
                            }
                        });

                        ActionTask* childPtr = child.get();
                        {
                            std::lock_guard<std::mutex> lock(childMutex);
                            childTasks.push_back(std::move(child));
                        }
                        taskGroup.AddTask(childPtr);
                        GetTaskScheduler()->AddTask(childPtr, TaskPriority::Normal);
                    }
                });

                taskGroup.AddTask(rootTask.get());
                scheduler.AddTask(rootTask.get(), TaskPriority::Normal);

                taskGroup.Wait();

                // Expected: 1 root + 8 children + 64 grandchildren + 256 great-grandchildren = 329 tasks
                Assert::AreEqual(static_cast<std::size_t>(329), totalExecuted.load(std::memory_order_acquire), L"All 329 dynamically spawned tree tasks must execute");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestStealTaskOrSuspendPriorityOrder)
        {
            const auto threadCounts = GetTestThreadCounts();
            LogDebugMessage(std::format(L"Starting TestStealTaskOrSuspendPriorityOrder for {} thread configuration(s)...", threadCounts.size()));

            for (DWORD numThreads : threadCounts) {
                LogDebugMessage(std::format(L"Testing StealTaskOrSuspend priority order with {} thread(s)", numThreads));
                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);

                auto normalTask = std::make_unique<ActionTask>([]() {});
                auto highTask   = std::make_unique<ActionTask>([]() {});

                scheduler.AddTask(normalTask.get(), TaskPriority::Normal);
                scheduler.AddTask(highTask.get(), TaskPriority::High);

                TaskPriority priority = TaskPriority::Normal;
                ITask* stolen1        = scheduler.StealTaskOrSuspend(priority);

                Assert::IsNotNull(stolen1, L"First stolen task should not be null");
                Assert::IsTrue(stolen1 == highTask.get(), L"StealTaskOrSuspend must return High priority task first");
                Assert::IsTrue(priority == TaskPriority::High, L"Priority output parameter must be High");

                ITask* stolen2 = scheduler.StealTaskOrSuspend(priority);

                Assert::IsNotNull(stolen2, L"Second stolen task should not be null");
                Assert::IsTrue(stolen2 == normalTask.get(), L"StealTaskOrSuspend must return Normal priority task second");
                Assert::IsTrue(priority == TaskPriority::Normal, L"Priority output parameter must be Normal");
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
