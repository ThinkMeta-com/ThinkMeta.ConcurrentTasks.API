#include "pch.h"
#include "TestHelpers.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <winrt/base.h>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.Await;
import ThinkMeta.ConcurrentTasks.Windows.Await;
import ThinkMeta.ConcurrentTasks.MediaFoundation.Await;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    namespace {
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

        // Calls f on a new thread after delay.
        void RunLater(std::chrono::milliseconds delay, std::function<void()> f)
        {
            std::thread([delay, f = std::move(f)] {
                std::this_thread::sleep_for(delay);
                f();
            }).detach();
        }

        // An event generator over an event queue, as Media Foundation objects implement it.
        class EventGenerator final : public IMFMediaEventGenerator
        {
        public:
            EventGenerator() { Assert::AreEqual(S_OK, MFCreateEventQueue(queue_.put())); }
            ~EventGenerator() { queue_->Shutdown(); }

            IMFMediaEventQueue* Queue() const noexcept { return queue_.get(); }

            STDMETHODIMP QueryInterface(REFIID iid, void** object) noexcept override
            {
                if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator)) {
                    *object = static_cast<IMFMediaEventGenerator*>(this);
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }
            // Lives on the test's stack; reference counting is not needed.
            STDMETHODIMP_(ULONG) AddRef() noexcept override { return 2; }
            STDMETHODIMP_(ULONG) Release() noexcept override { return 1; }

            STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) noexcept override { return queue_->GetEvent(flags, event); }
            STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) noexcept override { return queue_->BeginGetEvent(callback, state); }
            STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) noexcept override { return queue_->EndGetEvent(result, event); }
            STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status, const PROPVARIANT* value) noexcept override
            {
                return queue_->QueueEventParamVar(type, extendedType, status, value);
            }

        private:
            winrt::com_ptr<IMFMediaEventQueue> queue_;
        };
    } // namespace

    TEST_CLASS(AwaitTests)
    {
    public:
        TEST_CLASS_INITIALIZE(Initialize)
        {
            Assert::AreEqual(S_OK, CoIncrementMTAUsage(&mtaCookie_));
            Assert::AreEqual(S_OK, MFStartup(MF_VERSION, MFSTARTUP_LITE));
        }

        TEST_CLASS_CLEANUP(Cleanup)
        {
            MFShutdown();
            CoDecrementMTAUsage(mtaCookie_);
        }

        TEST_METHOD(TestAwaitCompletedBeforeStartReturns)
        {
            bool passed = false;
            RunInTask(1, [&passed] {
                Synchronization::Await([](Synchronization::Completion& completion) { completion(); });
                passed = true;
            });
            Assert::IsTrue(passed);
        }

        TEST_METHOD(TestAwaitCompletionFromAnotherThread)
        {
            std::atomic<bool> completed{false};
            RunInTask(1, [&completed] {
                Synchronization::Await([&completed](Synchronization::Completion& completion) {
                    RunLater(std::chrono::milliseconds(30), [&completed, completion] {
                        completed.store(true, std::memory_order_release);
                        completion();
                    });
                });
                Assert::IsTrue(completed.load(std::memory_order_acquire), L"Await returned before the completion");
            });
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

                std::atomic<int>                         finished{0};
                std::vector<std::unique_ptr<ActionTask>> tasks;
                tasks.reserve(NumTasks);

                const auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < NumTasks; ++i) {
                    tasks.push_back(std::make_unique<ActionTask>([Delay, &finished] {
                        Synchronization::Await([Delay](Synchronization::Completion& completion) { RunLater(Delay, [completion] { completion(); }); });
                        finished.fetch_add(1, std::memory_order_release);
                    }));
                    scheduler.AddTask(tasks.back().get(), TaskPriority::Normal);
                }

                const bool allFinished = WaitFor([&finished] { return finished.load(std::memory_order_acquire) == NumTasks; }, std::chrono::seconds(10));
                const auto elapsed     = std::chrono::steady_clock::now() - start;

                scheduler.Stop();

                Assert::IsTrue(allFinished, L"not every task finished");
                Assert::IsTrue(elapsed < NumTasks * Delay / 4, L"the waits did not overlap");
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestAwaitHandleSignaledLater)
        {
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            Assert::IsNotNull(event);

            Windows::ErrorCode error = Windows::Error::InvalidParameter;
            RunInTask(1, [event, &error] {
                RunLater(std::chrono::milliseconds(30), [event] { SetEvent(event); });
                error = Windows::Await(event);
                Assert::AreEqual(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(event, 0), L"Await returned before the event was set");
            });

            CloseHandle(event);
            Assert::AreEqual(Windows::Error::Success, error);
        }

        TEST_METHOD(TestAwaitHandleAlreadySignaledConsumesAutoReset)
        {
            HANDLE event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
            Assert::IsNotNull(event);

            Windows::ErrorCode error = Windows::Error::InvalidParameter;
            RunInTask(1, [event, &error] { error = Windows::Await(event); });

            Assert::AreEqual(Windows::Error::Success, error);
            Assert::AreEqual(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(event, 0), L"the auto-reset event should have been reset");
            CloseHandle(event);
        }

        TEST_METHOD(TestAwaitHandleOfFinishedThread)
        {
            std::thread worker;
            HANDLE      thread = nullptr;
            {
                worker = std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
                Assert::IsTrue(DuplicateHandle(GetCurrentProcess(), worker.native_handle(), GetCurrentProcess(), &thread, SYNCHRONIZE, FALSE, 0) != FALSE);
            }

            Windows::ErrorCode error = Windows::Error::InvalidParameter;
            RunInTask(1, [thread, &error] { error = Windows::Await(thread); });

            worker.join();
            CloseHandle(thread);
            Assert::AreEqual(Windows::Error::Success, error);
        }

        TEST_METHOD(TestAwaitMediaEvent)
        {
            EventGenerator generator;
            HRESULT        result = E_FAIL;
            MediaEventType type   = MEUnknown;
            RunInTask(1, [&generator, &result, &type] {
                RunLater(std::chrono::milliseconds(30), [&generator] { generator.Queue()->QueueEventParamVar(MEExtendedType, GUID_NULL, S_OK, nullptr); });

                winrt::com_ptr<IMFMediaEvent> event;
                result = MediaFoundation::Await(&generator, event.put());
                if (SUCCEEDED(result))
                    event->GetType(&type);
            });

            Assert::AreEqual(S_OK, result);
            Assert::AreEqual(static_cast<DWORD>(MEExtendedType), static_cast<DWORD>(type));
        }

        TEST_METHOD(TestAwaitMediaEventShutdown)
        {
            EventGenerator generator;
            HRESULT        result = S_OK;
            generator.Queue()->Shutdown();
            RunInTask(1, [&generator, &result] {
                winrt::com_ptr<IMFMediaEvent> event;
                result = MediaFoundation::Await(&generator, event.put());
            });

            Assert::AreEqual(MF_E_SHUTDOWN, result);
        }

    private:
        static inline CO_MTA_USAGE_COOKIE mtaCookie_ = nullptr;
    };

} // namespace ThinkMeta::ConcurrentTasks::Tests
