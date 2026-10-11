#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {
    using Tasks::Task;

    namespace {
        // Timers are process-wide; every test enables them for its own scope only, and disables them
        // before its scheduler goes away (a cancelled task ends on its scheduler).
        class TimersOn
        {
        public:
            TimersOn() { Assert::IsTrue(EnableTimers(), L"EnableTimers failed"); }
            ~TimersOn() { DisableTimers(); }

            TimersOn(const TimersOn&)            = delete;
            TimersOn& operator=(const TimersOn&) = delete;
        };

        double MillisecondsSince(std::chrono::steady_clock::time_point start) noexcept
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        struct ExitCounter : INotifyTaskStateChange {
            std::atomic<int> exits{0};
            void             OnTaskExited() noexcept override { exits.fetch_add(1); }
        };
    } // namespace

    // No Assert inside a task: it throws, and a task's Run is noexcept. Tasks record, the test asserts.
    TEST_CLASS(TimerTests)
    {
    public:
        TEST_METHOD(TestSleepingTaskLeavesItsThreadToOthers)
        {
            ConvertThreadToTask();
            {
                // One thread: the second task can only run while the first one sleeps.
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 1});
                scheduler.Start();
                TimersOn on;

                std::atomic<bool>   otherRan{false};
                std::atomic<bool>   otherRanWhileSleeping{false};
                std::atomic<bool>   sleepFailed{false};
                std::atomic<bool>   sleeperDone{false};
                std::atomic<double> slept{0.0};

                ActionTask sleeper([&]() {
                    const auto start = std::chrono::steady_clock::now();
                    sleepFailed.store(!Task::SleepFor(Duration::Milliseconds(50)));
                    slept.store(MillisecondsSince(start));
                    otherRanWhileSleeping.store(otherRan.load());
                    sleeperDone.store(true);
                });
                ActionTask other([&]() { otherRan.store(true); });

                scheduler.AddTask(&sleeper, TaskPriority::Normal);
                scheduler.AddTask(&other, TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&]() { return sleeperDone.load(); }), L"the sleeping task did not finish");
                Assert::IsFalse(sleepFailed.load(), L"SleepFor failed");
                Assert::IsTrue(otherRanWhileSleeping.load(), L"the other task did not run while the first one slept");
                Assert::IsTrue(slept.load() >= 50.0, L"the task slept too short");
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestThousandsOfSleepingTasks)
        {
            ConvertThreadToTask();
            {
                constexpr int   TaskCount = 5000;
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 4, .taskQueueCapacity = TaskCount});
                scheduler.Start();
                TimersOn on;

                std::atomic<int> finished{0};
                std::atomic<int> tooShort{0};

                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(TaskCount);
                for (int i = 0; i < TaskCount; ++i) {
                    const long long ms = 20 + i % 30;
                    tasks.push_back(std::make_unique<ActionTask>([&, ms]() {
                        const auto start = std::chrono::steady_clock::now();
                        if (!Task::SleepFor(Duration::Milliseconds(ms)) || MillisecondsSince(start) < static_cast<double>(ms))
                            tooShort.fetch_add(1);
                        finished.fetch_add(1);
                    }));
                }

                const auto start = std::chrono::steady_clock::now();
                for (auto& task : tasks)
                    scheduler.AddTask(task.get(), TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&]() { return finished.load() == TaskCount; }, std::chrono::seconds(20)), L"not every sleeping task finished");
                const double elapsed = MillisecondsSince(start);

                Assert::AreEqual(0, tooShort.load(), L"tasks woke before their time");
                // Slept one after another they would take minutes; together they take about the longest sleep.
                Assert::IsTrue(elapsed < 5000.0, L"the tasks did not sleep concurrently");
                LogDebugMessage(std::format(L"{} tasks sleeping 20..49 ms finished after {:.1f} ms", TaskCount, elapsed));
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestSleepForWithoutTimersDoesNotSleep)
        {
            ConvertThreadToTask();

            const auto start = std::chrono::steady_clock::now();
            Assert::IsFalse(Task::SleepFor(Duration::Seconds(10)), L"SleepFor must fail while timers are disabled");
            Assert::IsTrue(MillisecondsSince(start) < 1000.0, L"SleepFor slept although it failed");

            ConvertTaskToThread();
        }

        TEST_METHOD(TestScheduleCancelAndScheduleNow)
        {
            ConvertThreadToTask();
            {
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();
                TimersOn on;

                ExitCounter         laterExits, cancelledExits, nowExits;
                std::atomic<int>    laterRuns{0}, cancelledRuns{0}, nowRuns{0};
                std::atomic<double> laterAfter{0.0};
                const auto          start = std::chrono::steady_clock::now();

                ActionTask later([&]() { laterAfter.store(MillisecondsSince(start)); laterRuns.fetch_add(1); }, &laterExits);
                ActionTask cancelled([&]() { cancelledRuns.fetch_add(1); }, &cancelledExits);
                ActionTask now([&]() { nowRuns.fetch_add(1); }, &nowExits);

                std::atomic<bool> scheduled{false};
                std::atomic<bool> cancelResult{false};
                std::atomic<bool> nowResult{false};

                // Schedule uses the calling task's scheduler, so a task does the scheduling.
                ActionTask planner([&]() {
                    later.Schedule(TaskPriority::Normal, Duration::Milliseconds(30));
                    cancelled.Schedule(TaskPriority::Normal, Duration::Seconds(60));
                    now.Schedule(TaskPriority::Normal, Duration::Seconds(60));
                    cancelResult.store(cancelled.CancelSchedule());
                    nowResult.store(now.ScheduleNow());
                    scheduled.store(true);
                });
                scheduler.AddTask(&planner, TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&]() { return scheduled.load(); }), L"the planning task did not run");
                Assert::IsTrue(cancelResult.load(), L"CancelSchedule failed");
                Assert::IsTrue(nowResult.load(), L"ScheduleNow failed");

                Assert::IsTrue(WaitFor([&]() { return laterExits.exits.load() == 1 && cancelledExits.exits.load() == 1 && nowExits.exits.load() == 1; }),
                               L"not every task exited");
                Assert::AreEqual(1, laterRuns.load(), L"the scheduled task must run");
                Assert::IsTrue(laterAfter.load() >= 30.0, L"the scheduled task started early");
                Assert::AreEqual(0, cancelledRuns.load(), L"the cancelled task ran");
                Assert::AreEqual(1, nowRuns.load(), L"the task started now must run");
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestWaitWithTimeoutOnSingleWaiterPrimitives)
        {
            ConvertThreadToTask();
            {
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();
                TimersOn on;

                Synchronization::AutoResetEvent_s event;
                Synchronization::Semaphore_s      semaphore;

                std::atomic<int>    eventTimedOut{-1}, eventSignaled{-1}, semaphoreTimedOut{-1}, semaphoreSignaled{-1};
                std::atomic<double> eventWaited{0.0};
                std::atomic<bool>   waiting{false};

                ActionTask waiter([&]() {
                    // nobody sets the event: the wait times out
                    const auto start = std::chrono::steady_clock::now();
                    eventTimedOut.store(event.Wait(Duration::Milliseconds(30)) ? 1 : 0);
                    eventWaited.store(MillisecondsSince(start));

                    // now the test sets it while the task waits
                    waiting.store(true);
                    eventSignaled.store(event.Wait(Duration::Seconds(10)) ? 1 : 0);

                    semaphoreTimedOut.store(semaphore.Wait(Duration::Milliseconds(10)) ? 1 : 0);
                    semaphore.Release();
                    semaphoreSignaled.store(semaphore.Wait(Duration::Seconds(10)) ? 1 : 0);
                });
                scheduler.AddTask(&waiter, TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&]() { return waiting.load(); }), L"the waiting task did not get to its second wait");
                Sleep(20); // let it suspend
                event.Set();

                Assert::IsTrue(WaitFor([&]() { return semaphoreSignaled.load() != -1; }), L"the waiting task did not finish");
                Assert::AreEqual(0, eventTimedOut.load(), L"an event nobody sets must time out");
                Assert::IsTrue(eventWaited.load() >= 30.0, L"the event wait timed out early");
                Assert::AreEqual(1, eventSignaled.load(), L"a set event must end the wait");
                Assert::AreEqual(0, semaphoreTimedOut.load(), L"a semaphore without count must time out");
                Assert::AreEqual(1, semaphoreSignaled.load(), L"a released semaphore must end the wait");
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestWaitForATaskWithTimeout)
        {
            ConvertThreadToTask();
            {
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();
                TimersOn on;

                // a task that runs until it is let go
                struct Slow : Tasks::WaitableTask_s {
                    std::atomic<bool> release{false};

                    void Run() noexcept override
                    {
                        while (!release.load())
                            Task::SleepFor(Duration::Milliseconds(1));
                    }
                } slow;

                std::atomic<int> firstWait{-1}, secondWait{-1};

                ActionTask waiter([&]() {
                    firstWait.store(slow.Wait(Duration::Milliseconds(20)) ? 1 : 0); // still running
                    slow.release.store(true);
                    secondWait.store(slow.Wait(Duration::Seconds(10)) ? 1 : 0);
                });

                scheduler.AddTask(&slow, TaskPriority::Normal);
                scheduler.AddTask(&waiter, TaskPriority::Normal);

                Assert::IsTrue(WaitFor([&]() { return secondWait.load() != -1; }), L"the waiting task did not finish");
                Assert::AreEqual(0, firstWait.load(), L"waiting for a running task must time out");
                Assert::AreEqual(1, secondWait.load(), L"waiting for a task that exits must succeed");
            }
            ConvertTaskToThread();
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
