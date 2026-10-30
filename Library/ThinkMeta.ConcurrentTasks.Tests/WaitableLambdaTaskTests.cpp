#include "pch.h"
#include "TestHelpers.h"

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.RunAndWait;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    TEST_CLASS(WaitableLambdaTaskTests)
    {
    public:
        TEST_METHOD(TestCreateReturnsATaskThatCanBeWaitedFor)
        {
            ConvertThreadToTask();
            {
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();

                std::atomic<int> runs{0};
                auto             multiple = Tasks::WaitableTask::Create([&] { runs.fetch_add(1); });
                auto             single   = Tasks::WaitableTask_s::Create([&] { runs.fetch_add(1); });
                static_assert(std::is_base_of_v<Tasks::WaitableTask, decltype(multiple)>, "WaitableTask::Create is not waitable");
                static_assert(std::is_base_of_v<Tasks::WaitableTask_s, decltype(single)>, "WaitableTask_s::Create is not waitable");
                multiple.Schedule(*scheduler.Get());
                single.Schedule(*scheduler.Get());
                multiple.Wait();
                Assert::IsTrue(single.Wait(Duration::Seconds(10)), L"the single-waiter task did not end");
                Assert::AreEqual(2, runs.load(), L"not both tasks ran");
                Assert::IsTrue(multiple.TryWait() && single.TryWait(), L"a finished task does not report it");

                scheduler.Stop();
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestRunAndWaitDoesNotCopyTheCallable)
        {
            struct Counter {
                int* calls;
                int* copies;
                Counter(int* c, int* k) : calls(c), copies(k) {}
                Counter(const Counter& other) : calls(other.calls), copies(other.copies) { ++*copies; }
                void operator()() const noexcept { ++*calls; }
            };

            ConvertThreadToTask();
            {
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();

                int     calls  = 0;
                int     copies = 0;
                Counter counter(&calls, &copies);
                Tasks::RunAndWait(*scheduler.Get(), counter);
                Assert::AreEqual(1, calls, L"the callable did not run once");
                Assert::AreEqual(0, copies, L"RunAndWait copied the callable");

                scheduler.Stop();
            }
            ConvertTaskToThread();
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
