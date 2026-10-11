#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(TaskGroupTests)
    {
    public:
        TEST_METHOD(TestTaskGroupMultipleTasks)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumGroupTasks = 64;

            LogDebugMessage(std::format(L"Starting TaskGroup multiple tasks test for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<std::size_t> executedCount{0};

                std::vector<std::unique_ptr<ActionTask>> groupTasks;
                groupTasks.reserve(NumGroupTasks);

                for (std::size_t i = 0; i < NumGroupTasks; ++i) {
                    groupTasks.push_back(std::make_unique<ActionTask>([&executedCount]() {
                        SimulateCpuLoad(std::chrono::milliseconds(2));
                        for (std::size_t step = 0; step < 10; ++step) {
                            GetTaskScheduler()->YieldTask();
                        }
                        executedCount.fetch_add(1, std::memory_order_release);
                    }));
                }

                Synchronization::TaskGroup taskGroup;
                for (std::size_t i = 0; i < NumGroupTasks; ++i) {
                    taskGroup.AddTask(groupTasks[i].get());
                    scheduler.AddTask(groupTasks[i].get(), TaskPriority::Normal);
                }

                // Wait for all tasks in the group to complete
                taskGroup.Wait();

                Assert::AreEqual(NumGroupTasks, executedCount.load(std::memory_order_acquire), L"All tasks in TaskGroup should have completed before Wait returns");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTaskGroupSingleWaiter)
        {
            const unsigned int maxThreads = std::max(1u, std::thread::hardware_concurrency());
            constexpr std::size_t NumGroupTasks = 32;

            LogDebugMessage(std::format(L"Starting TaskGroup_s (single-waiter) test for 1..{} threads.", maxThreads));

            for (unsigned int numThreads = 1; numThreads <= maxThreads; ++numThreads) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<std::size_t> executedCount{0};

                std::vector<std::unique_ptr<ActionTask>> groupTasks;
                groupTasks.reserve(NumGroupTasks);

                for (std::size_t i = 0; i < NumGroupTasks; ++i) {
                    groupTasks.push_back(std::make_unique<ActionTask>([&executedCount]() {
                        SimulateCpuLoad(std::chrono::milliseconds(2));
                        for (std::size_t step = 0; step < 10; ++step) {
                            GetTaskScheduler()->YieldTask();
                        }
                        executedCount.fetch_add(1, std::memory_order_release);
                    }));
                }

                Synchronization::TaskGroup_s taskGroup;
                for (std::size_t i = 0; i < NumGroupTasks; ++i) {
                    taskGroup.AddTask(groupTasks[i].get());
                    scheduler.AddTask(groupTasks[i].get(), TaskPriority::Normal);
                }

                taskGroup.Wait();

                Assert::AreEqual(NumGroupTasks, executedCount.load(std::memory_order_acquire), L"All tasks in TaskGroup_s must complete before Wait returns");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTaskGroupTryWait)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    Synchronization::TaskGroup taskGroup;
                    Assert::IsTrue(taskGroup.TryWait(), L"Empty TaskGroup TryWait must return true");

                    std::atomic<bool> subTaskDone{false};
                    auto subTask = Tasks::Task::Create([&subTaskDone]() {
                        SimulateCpuLoad(std::chrono::milliseconds(50));
                        subTaskDone.store(true, std::memory_order_release);
                    });

                    taskGroup.AddTask(&subTask);
                    GetTaskScheduler()->AddTask(&subTask, TaskPriority::Normal);

                    // TryWait while task is active
                    Assert::IsFalse(taskGroup.TryWait(), L"TryWait on active TaskGroup must return false");

                    taskGroup.Wait();
                    Assert::IsTrue(taskGroup.TryWait(), L"TryWait after completion must return true");

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"TaskGroup TryWait test task timed out!");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTaskGroupDynamicAdd)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Synchronization::TaskGroup taskGroup;
                std::atomic<std::size_t> dynamicTaskCount{0};

                // Dynamically spawned child tasks must outlive taskGroup.Wait(); keep them
                // alive in an outer-scope container rather than as locals inside the parent
                // task (which would be destroyed while still queued in the scheduler).
                std::vector<std::unique_ptr<ActionTask>> childTasks;
                std::mutex childTasksMutex;

                auto initialTask = Tasks::Task::Create([&taskGroup, &dynamicTaskCount, &childTasks, &childTasksMutex]() {
                    dynamicTaskCount.fetch_add(1, std::memory_order_release);

                    // Dynamically spawn child tasks into the same TaskGroup
                    auto childTask1 = std::make_unique<ActionTask>([&dynamicTaskCount]() {
                        dynamicTaskCount.fetch_add(1, std::memory_order_release);
                    });

                    ActionTask* childPtr = childTask1.get();
                    {
                        std::lock_guard<std::mutex> lock(childTasksMutex);
                        childTasks.push_back(std::move(childTask1));
                    }

                    taskGroup.AddTask(childPtr);
                    GetTaskScheduler()->AddTask(childPtr, TaskPriority::Normal);
                });

                taskGroup.AddTask(&initialTask);
                scheduler.AddTask(&initialTask, TaskPriority::Normal);

                taskGroup.Wait();

                Assert::AreEqual(static_cast<std::size_t>(2), dynamicTaskCount.load(std::memory_order_acquire), L"Dynamic tasks added to TaskGroup should be waited on");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTaskGroupEmptyWait)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&taskDone]() {
                    // Waiting on a group with no tasks must return immediately.
                    Synchronization::TaskGroup emptyGroup;
                    emptyGroup.Wait();

                    // Single-waiter variant must behave the same way.
                    Synchronization::TaskGroup_s emptySingleGroup;
                    emptySingleGroup.Wait();

                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); });
                Assert::IsTrue(finished, L"Empty TaskGroup Wait() did not return.");

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        // Fork/join with the group as a local object: the function returns (and destroys the group)
        // right after Wait, while the last task's exit notification or the waiter's own re-check may
        // still be running on another thread.
        template <class Group>
        static void ForkJoinWithLocalGroup(std::size_t taskCount, std::atomic<std::size_t>& work)
        {
            Group group;
            std::vector<std::unique_ptr<ActionTask>> tasks;
            tasks.reserve(taskCount);
            for (std::size_t i = 0; i < taskCount; ++i) {
                tasks.push_back(std::make_unique<ActionTask>([&work]() { work.fetch_add(1, std::memory_order_relaxed); }));
                group.AddTask(tasks.back().get());
                GetTaskScheduler()->AddTask(tasks.back().get(), TaskPriority::Normal);
            }
            group.Wait();
        }

        template <class Group>
        static void CheckLocalGroupLifetime(const wchar_t* message)
        {
            constexpr std::size_t Rounds = 10000;

            ConvertThreadToTask();

            SchedulerInfo   schedulerInfo = {.numberOfThreads = std::max(2u, std::thread::hardware_concurrency())};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            std::atomic<std::size_t> work{0};
            std::atomic<std::size_t> expected{0};
            std::atomic<bool>        done{false};

            auto driver = Tasks::Task::Create([&]() {
                for (std::size_t round = 0; round < Rounds; ++round) {
                    const std::size_t taskCount = 1 + round % 16;
                    expected.fetch_add(taskCount, std::memory_order_relaxed);
                    ForkJoinWithLocalGroup<Group>(taskCount, work);
                }
                done.store(true, std::memory_order_release);
            });
            scheduler.AddTask(&driver, TaskPriority::Normal);

            const bool finished = WaitFor([&done] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(120));
            Assert::IsTrue(finished, message);
            Assert::AreEqual(expected.load(), work.load(), L"not every task of every round ran");

            scheduler.Stop();
            ConvertTaskToThread();
        }

        TEST_METHOD(TestTaskGroupAsLocalObject)
        {
            CheckLocalGroupLifetime<Synchronization::TaskGroup>(L"fork/join with a local TaskGroup did not finish");
        }

        TEST_METHOD(TestTaskGroupSingleWaiterAsLocalObject)
        {
            CheckLocalGroupLifetime<Synchronization::TaskGroup_s>(L"fork/join with a local TaskGroup_s did not finish");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
