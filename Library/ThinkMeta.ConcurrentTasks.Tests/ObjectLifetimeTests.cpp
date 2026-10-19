#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// A waiter may destroy the object it waited on as soon as the wait is over - the I/O request of a
// socket call, a TaskGroup on the stack. Nothing in the library may touch the object after the
// operation that let the waiter pass. Each object here lives on a page of its own that the waiter
// releases right after its wait, so any late access faults.
// Many waiters run at once, each a scheduled task (a converted thread runs OnSuspended itself before
// it blocks), and the signal comes before, during or after the suspension.

namespace ThinkMeta::ConcurrentTasks::Tests {

    namespace {
        constexpr int Waiters = 8;
        constexpr int Rounds  = 20000;

        /// <summary>
        /// Rounds of: object on a fresh page, a member task that signals it (prepare runs before the
        /// member is scheduled), the waiter's wait, destruction, release of the page.
        /// </summary>
        template <typename Object, typename Prepare, typename Signal, typename Await>
        void RunLifetimeRounds(Prepare prepare, Signal signal, Await await)
        {
            ConvertThreadToTask();
            SchedulerInfo   schedulerInfo = {.numberOfThreads = 16};
            ScopedScheduler scheduler(schedulerInfo);
            scheduler.Start();

            std::vector<std::vector<std::unique_ptr<ActionTask>>> members(Waiters);
            for (auto& list : members)
                list.reserve(Rounds);

            std::atomic<int>                         done{0};
            std::atomic<bool>                        failed{false};
            std::vector<std::unique_ptr<ActionTask>> waiters;
            for (int w = 0; w < Waiters; ++w) {
                waiters.push_back(std::make_unique<ActionTask>([&, w]() {
                    for (int round = 0; round < Rounds; ++round) {
                        void* const page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                        if (!page) {
                            failed.store(true);
                            break;
                        }
                        Object* const object = new (page) Object;
                        const int     spin   = round % 97;
                        members[w].push_back(std::make_unique<ActionTask>([object, spin, &signal]() {
                            for (int i = 0; i < spin; ++i)
                                YieldProcessor();
                            signal(*object);
                        }));
                        ActionTask* const member = members[w].back().get();
                        prepare(*object, member);
                        GetTaskScheduler()->AddTask(member, TaskPriority::Normal);
                        await(*object, round);
                        std::destroy_at(object);
                        VirtualFree(page, 0, MEM_RELEASE);
                    }
                    done.fetch_add(1, std::memory_order_release);
                }));
                scheduler.AddTask(waiters.back().get(), TaskPriority::Normal);
            }

            const bool finished = WaitFor([&done] { return done.load(std::memory_order_acquire) == Waiters; }, std::chrono::seconds(120));
            Assert::IsTrue(finished, L"the waiters did not finish");
            Assert::IsFalse(failed.load(), L"VirtualAlloc failed");

            scheduler.Stop();
            ConvertTaskToThread();
        }

        constexpr auto NoPreparation = [](auto&, ActionTask*) {};

        // a waitable task that exits at once; the member task schedules it. The single-waiter
        // variant: only its exit lets the waiter go in one operation, so only it may be destroyed
        // right after the wait (WaitableTask.ixx).
        struct ExitingTask final : Tasks::WaitableTask_s {
            void Run() noexcept override {}
        };
    } // namespace

    TEST_CLASS(ObjectLifetimeTests)
    {
    public:
        TEST_METHOD(TestManualResetEventFreedAfterWait)
        {
            // the pattern of every I/O request: the completion sets the event, the caller's frame goes
            using Event = Synchronization::ManualResetEvent_s;
            RunLifetimeRounds<Event>(NoPreparation, [](Event& event) { event.Set(); }, [](Event& event, int) { event.Wait(); });
        }

        TEST_METHOD(TestManualResetEventFreedAfterTimedWait)
        {
            // the timeout and Set race for the waiter; a waiter that timed out waits for the Set it
            // knows is coming before it frees the event (the signaller must find the event alive)
            Assert::IsTrue(EnableTimers(), L"EnableTimers failed");
            using Event = Synchronization::ManualResetEvent_s;
            RunLifetimeRounds<Event>(NoPreparation, [](Event& event) { event.Set(); },
                                     [](Event& event, int round) {
                                         if (!event.Wait(Duration::Microseconds(round % 50)))
                                             event.Wait();
                                     });
            DisableTimers();
        }

        TEST_METHOD(TestWaitableTaskSingleWaiterFreedAfterWait)
        {
            // the waiter owns the task object and lets it go as soon as Wait returned; the exit's
            // Set must not touch the object after resuming it
            RunLifetimeRounds<ExitingTask>(NoPreparation, [](ExitingTask& task) { GetTaskScheduler()->AddTask(&task, TaskPriority::Normal); },
                                           [](ExitingTask& task, int) { task.Wait(); });
        }

        TEST_METHOD(TestTaskGroupFreedAfterWait)
        {
            // a group on the waiter's stack: the member's exit counts it out and resumes the waiter
            using Group = Synchronization::TaskGroup;
            RunLifetimeRounds<Group>([](Group& group, ActionTask* member) { group.AddTask(member); }, [](Group&) {}, [](Group& group, int) { group.Wait(); });
        }

        TEST_METHOD(TestAutoResetEventFreedAfterWait)
        {
            using Event = Synchronization::AutoResetEvent_s;
            RunLifetimeRounds<Event>(NoPreparation, [](Event& event) { event.Set(); }, [](Event& event, int) { event.Wait(); });
        }

        TEST_METHOD(TestAutoResetEventFreedAfterTimedWait)
        {
            // as for the manual-reset event: a waiter that timed out waits for the Set that is coming
            Assert::IsTrue(EnableTimers(), L"EnableTimers failed");
            using Event = Synchronization::AutoResetEvent_s;
            RunLifetimeRounds<Event>(NoPreparation, [](Event& event) { event.Set(); },
                                     [](Event& event, int round) {
                                         if (!event.Wait(Duration::Microseconds(round % 50)))
                                             event.Wait();
                                     });
            DisableTimers();
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
