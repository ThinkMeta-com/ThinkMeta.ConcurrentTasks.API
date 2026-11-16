#include "pch.h"
#include "TestHelpers.h"

#include <atomic>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Schedulers.LocalScheduler;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.RunAndWait;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {
    TEST_CLASS(LocalSchedulerTests)
    {
    public:
        TEST_METHOD(TestTasksRunOnTheCallingThreadAndRunReturnsWhenAllEnded)
        {
            Schedulers::LocalScheduler local;
            Assert::IsTrue(static_cast<bool>(local), L"the local scheduler was not created");

            const DWORD      caller = GetCurrentThreadId();
            std::atomic<int> ran{0};
            std::atomic<int> wrongThread{0};

            auto body = [&] {
                if (GetCurrentThreadId() != caller)
                    wrongThread.fetch_add(1);
                ran.fetch_add(1);
            };
            auto child = Tasks::Task::Create(body);
            auto first = Tasks::Task::Create([&] {
                body();
                child.Schedule(); // on the local scheduler too: the calling task's own
            });
            auto second = Tasks::Task::Create(body);

            first.Schedule(local);
            local.AddTask(&second);
            local.Run();

            Assert::AreEqual(3, ran.load(), L"Run returned before every task had ended");
            Assert::AreEqual(0, wrongThread.load(), L"a task ran on another thread");
        }

        TEST_METHOD(TestRunAndWaitOnWorkersContinuesOnTheCallingThread)
        {
            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 4});
            workers.Start();
            Schedulers::LocalScheduler local;

            const DWORD caller      = GetCurrentThreadId();
            DWORD       workThread  = 0;
            DWORD       threadAfter = 0;

            auto task = Tasks::Task::Create([&] {
                Tasks::RunAndWait(*workers.Get(), [&] { workThread = GetCurrentThreadId(); });
                threadAfter = GetCurrentThreadId();
            });
            task.Schedule(local);
            local.Run();

            Assert::IsTrue(workThread != 0 && workThread != caller, L"the work did not run on a worker");
            Assert::IsTrue(threadAfter == caller, L"the task did not continue on the calling thread");
        }

        TEST_METHOD(TestTaskGroupsOnWorkersWhileOtherLocalTasksRun)
        {
            constexpr int Groups = 4;
            constexpr int Jobs   = 16;

            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 4});
            workers.Start();
            Schedulers::LocalScheduler local;

            const DWORD      caller = GetCurrentThreadId();
            std::atomic<int> jobsDone{0};
            std::atomic<int> groupsDone{0};
            std::atomic<int> wrongThread{0};

            auto job = [&] {
                volatile int sink = 0;
                for (int i = 0; i < 20000; ++i)
                    sink = sink + i;
                jobsDone.fetch_add(1);
            };
            auto groupBody = [&] {
                // each local task forks onto the workers and waits; the others run meanwhile
                Synchronization::TaskGroup_s group;
                auto                         a = Tasks::Task::Create(job);
                auto                         b = Tasks::Task::Create(job);
                auto                         c = Tasks::Task::Create(job);
                auto                         d = Tasks::Task::Create(job);
                for (int round = 0; round < Jobs / 4; ++round) {
                    for (auto* t : {&a, &b, &c, &d}) {
                        group.AddTask(t);
                        t->Schedule(*workers.Get());
                    }
                    group.Wait();
                }
                if (GetCurrentThreadId() != caller)
                    wrongThread.fetch_add(1);
                groupsDone.fetch_add(1);
            };
            auto g0 = Tasks::Task::Create(groupBody);
            auto g1 = Tasks::Task::Create(groupBody);
            auto g2 = Tasks::Task::Create(groupBody);
            auto g3 = Tasks::Task::Create(groupBody);
            for (auto* g : {&g0, &g1, &g2, &g3})
                g->Schedule(local);
            local.Run();

            Assert::AreEqual(Groups, groupsDone.load());
            Assert::AreEqual(Groups * Jobs, jobsDone.load());
            Assert::AreEqual(0, wrongThread.load(), L"a local task continued on another thread");
        }

        TEST_METHOD(TestSchedulerMayGoRightAfterRun)
        {
            // The scheduler is a local variable that goes as soon as Run has returned, while the
            // workers that resumed its tasks are still on their way out of it. Many rounds, with the
            // calling thread busy meanwhile, so that it does not sleep when a worker resumes.
            constexpr int Rounds = 2000;

            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 4});
            workers.Start();

            std::atomic<int> done{0};
            for (int round = 0; round < Rounds; ++round) {
                auto handOver = [&] {
                    Tasks::RunAndWait(*workers.Get(), [] {});
                    done.fetch_add(1);
                };
                auto busy = [&] {
                    volatile int sink = 0;
                    for (int i = 0; i < 2000; ++i)
                        sink = sink + i;
                    done.fetch_add(1);
                };
                auto t0 = Tasks::Task::Create(handOver);
                auto t1 = Tasks::Task::Create(handOver);
                auto t2 = Tasks::Task::Create(busy);

                Schedulers::LocalScheduler local;
                t0.Schedule(local);
                t1.Schedule(local);
                t2.Schedule(local);
                local.Run();
            }

            Assert::AreEqual(3 * Rounds, done.load());
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
