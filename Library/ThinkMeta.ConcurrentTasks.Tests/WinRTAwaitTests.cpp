#include "pch.h"
#include "TestHelpers.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.Threading.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.WinRT.Await;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    namespace Foundation = winrt::Windows::Foundation;

    static_assert(WinRT::AsyncOperationType<Foundation::IAsyncAction>);
    static_assert(WinRT::AsyncOperationType<Foundation::IAsyncOperation<int>>);
    static_assert(WinRT::AsyncOperationType<Foundation::IAsyncActionWithProgress<int>>);
    static_assert(WinRT::AsyncOperationType<Foundation::IAsyncOperationWithProgress<int, int>>);
    static_assert(!WinRT::AsyncOperationType<int>);

    namespace {
        Foundation::IAsyncOperation<int> DelayedValueAsync(int value, std::chrono::milliseconds delay)
        {
            co_await winrt::resume_after(delay);
            co_return value;
        }

        Foundation::IAsyncOperation<int> ImmediateValueAsync(int value) { co_return value; }

        Foundation::IAsyncAction FailingAsync()
        {
            co_await winrt::resume_background();
            throw winrt::hresult_error(E_ACCESSDENIED);
        }

        // Runs body as one task on a scheduler with numThreads threads and waits for it.
        void RunInTask(std::uint32_t numThreads, std::function<void()> body)
        {
            ConvertThreadToTask();
            {
                SchedulerInfo   schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<bool> done{false};
                ActionTask        task([&body, &done] {
                    body();
                    done.store(true, std::memory_order_release);
                });
                scheduler.AddTask(&task, TaskPriority::Normal);

                const bool finished = WaitFor([&done] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(10));
                Assert::IsTrue(finished, L"the task did not finish");

                scheduler.Stop();
            }
            ConvertTaskToThread();
        }
    } // namespace

    TEST_CLASS(WinRTAwaitTests)
    {
    public:
        // The scheduler threads join the multithreaded apartment implicitly while it is kept alive.
        TEST_CLASS_INITIALIZE(InitializeMta) { Assert::AreEqual(S_OK, CoIncrementMTAUsage(&mtaCookie_)); }
        TEST_CLASS_CLEANUP(ReleaseMta) { CoDecrementMTAUsage(mtaCookie_); }

        TEST_METHOD(TestAwaitAction)
        {
            std::atomic<bool> ran{false};
            RunInTask(1, [&ran] {
                WinRT::Await(winrt::Windows::System::Threading::ThreadPool::RunAsync([&ran](auto&&) {
                    ::Sleep(30);
                    ran.store(true, std::memory_order_release);
                }));
                Assert::IsTrue(ran.load(std::memory_order_acquire), L"Await returned before the action ran");
            });
        }

        TEST_METHOD(TestAwaitOperationResult)
        {
            int result = 0;
            RunInTask(1, [&result] { result = WinRT::Await(DelayedValueAsync(42, std::chrono::milliseconds(30))); });
            Assert::AreEqual(42, result);
        }

        TEST_METHOD(TestAwaitCompletedOperation)
        {
            int result = 0;
            RunInTask(1, [&result] {
                const auto op = ImmediateValueAsync(7);
                Assert::IsTrue(op.Status() == Foundation::AsyncStatus::Completed, L"the operation should have completed already");
                result = WinRT::Await(op);
            });
            Assert::AreEqual(7, result);
        }

        TEST_METHOD(TestAwaitFailedOperationThrows)
        {
            HRESULT error = S_OK;
            RunInTask(1, [&error] {
                try {
                    WinRT::Await(FailingAsync());
                } catch (const winrt::hresult_error& e) {
                    error = e.code();
                }
            });
            Assert::AreEqual(E_ACCESSDENIED, error);
        }

        // Many tasks wait at once on one scheduler thread: the waits overlap only if Await
        // suspends the task instead of blocking the thread.
        TEST_METHOD(TestAwaitSuspendsTask)
        {
            constexpr int  NumTasks = 32;
            constexpr auto Delay    = std::chrono::milliseconds(100);

            ConvertThreadToTask();
            {
                SchedulerInfo   schedulerInfo = {.numberOfThreads = 1};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                std::atomic<int>                         sum{0};
                std::atomic<int>                         finished{0};
                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(NumTasks);

                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < NumTasks; ++i) {
                    tasks.push_back(std::make_unique<ActionTask>([i, Delay, &sum, &finished] {
                        sum.fetch_add(WinRT::Await(DelayedValueAsync(i, Delay)));
                        finished.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(tasks.back().get(), TaskPriority::Normal);
                }

                const bool allFinished = WaitFor([&finished] { return finished.load(std::memory_order_acquire) == NumTasks; }, std::chrono::seconds(10));
                const auto elapsed     = std::chrono::steady_clock::now() - start;

                scheduler.Stop();

                Assert::IsTrue(allFinished, L"not every task finished");
                Assert::AreEqual(NumTasks * (NumTasks - 1) / 2, sum.load());
                LogDebugMessage(std::format(L"{} overlapping waits of {} ms took {} ms", NumTasks, Delay.count(),
                                            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
                Assert::IsTrue(elapsed < NumTasks * Delay / 4, L"the waits did not overlap");
            }
            ConvertTaskToThread();
        }

    private:
        static inline CO_MTA_USAGE_COOKIE mtaCookie_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Tests
