#include "pch.h"
#include "TestHelpers.h"

#include <memory>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Schedulers.DispatcherScheduler;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.RunAndWait;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {
    namespace {
        constexpr UINT WM_DISPATCH = WM_APP + 1;

        // A message-only window whose procedure continues the tasks the dispatcher posts to it.
        class DispatchWindow
        {
        public:
            DispatchWindow() noexcept
            {
                WNDCLASSEXW wc{sizeof(wc)};
                wc.lpfnWndProc   = &DispatchWindow::Proc;
                wc.hInstance     = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"CtLibraryDispatcherTestWindow";
                RegisterClassExW(&wc);
                hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
                Assert::IsNotNull(hwnd_, L"the message window was not created");
                SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            }

            ~DispatchWindow()
            {
                DestroyWindow(hwnd_);
                UnregisterClassW(L"CtLibraryDispatcherTestWindow", GetModuleHandleW(nullptr));
            }

            HWND Handle() const noexcept { return hwnd_; }
            void SetDispatcher(Schedulers::DispatcherScheduler* dispatcher) noexcept { dispatcher_ = dispatcher; }

            // Dispatches messages until done() holds; false on timeout.
            template <class Done>
            static bool PumpUntil(Done done, DWORD milliseconds = 10000)
            {
                const ULONGLONG end = GetTickCount64() + milliseconds;
                MSG             msg{};
                while (!done()) {
                    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                        DispatchMessageW(&msg);
                    else if (GetTickCount64() > end)
                        return false;
                    else
                        Sleep(1);
                }
                return true;
            }

        private:
            static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
            {
                if (message == WM_DISPATCH) {
                    reinterpret_cast<DispatchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))->dispatcher_->ContinueTask(lParam);
                    return 0;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }

            HWND                             hwnd_       = nullptr;
            Schedulers::DispatcherScheduler* dispatcher_ = nullptr;
        };
    } // namespace

    TEST_CLASS(DispatcherSchedulerTests)
    {
    public:
        TEST_METHOD(TestRunAndWaitRunsOnTheWorkersAndContinuesOnTheUiThread)
        {
            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 4});
            workers.Start();
            DispatchWindow                  window;
            Schedulers::DispatcherScheduler dispatcher(window.Handle(), WM_DISPATCH);
            Assert::IsTrue(static_cast<bool>(dispatcher), L"the dispatcher scheduler was not created");
            window.SetDispatcher(&dispatcher);

            const DWORD       uiThread     = GetCurrentThreadId();
            DWORD             workThread   = 0;
            DWORD             threadAfter  = 0;
            std::atomic<bool> handlerEnded = false;
            auto              handler      = Tasks::Task::Create([&] {
                Tasks::RunAndWait(*workers.Get(), [&] { workThread = GetCurrentThreadId(); });
                threadAfter = GetCurrentThreadId();
                handlerEnded.store(true);
            });

            dispatcher.RunTask(&handler);
            Assert::IsTrue(DispatchWindow::PumpUntil([&] { return handlerEnded.load(); }), L"the handler did not continue");
            Assert::IsTrue(workThread != 0 && workThread != uiThread, L"the work ran on the UI thread");
            Assert::IsTrue(threadAfter == uiThread, L"the handler continued off the UI thread");
        }

        TEST_METHOD(TestHandlersSpreadWorkOverTheWorkersAndWaitForTheGroup)
        {
            constexpr int Handlers = 8;
            constexpr int Jobs     = 32;

            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 4});
            workers.Start();
            DispatchWindow                  window;
            Schedulers::DispatcherScheduler dispatcher(window.Handle(), WM_DISPATCH);
            window.SetDispatcher(&dispatcher);

            const DWORD      uiThread = GetCurrentThreadId();
            std::atomic<int> jobsDone{0};
            std::atomic<int> handlersEnded{0};
            std::atomic<int> wrongThread{0};

            struct Handler {
                std::vector<std::unique_ptr<ActionTask>> jobs;
                std::unique_ptr<ActionTask>              task;
            };
            Handler handlers[Handlers];
            for (auto& h : handlers) {
                for (int j = 0; j < Jobs; ++j)
                    h.jobs.push_back(std::make_unique<ActionTask>(std::function<void()>([&, j] {
                        volatile int sink = 0;
                        for (int i = 0; i < 20000 * (j % 4 + 1); ++i)
                            sink = sink + i;
                        jobsDone.fetch_add(1);
                    })));
                h.task = std::make_unique<ActionTask>(std::function<void()>([&, hp = &h] {
                    Synchronization::TaskGroup_s group;
                    for (auto& job : hp->jobs) {
                        group.AddTask(job.get());
                        job->Schedule(*workers.Get());
                    }
                    group.Wait();
                    if (GetCurrentThreadId() != uiThread)
                        wrongThread.fetch_add(1);
                    handlersEnded.fetch_add(1);
                }));
            }

            // as a burst of clicks: each handler waits while the next one starts
            for (auto& h : handlers)
                dispatcher.RunTask(h.task.get());

            Assert::IsTrue(DispatchWindow::PumpUntil([&] { return handlersEnded.load() == Handlers; }), L"not every handler continued");
            Assert::AreEqual(Handlers * Jobs, jobsDone.load(), L"a handler continued before its jobs were done");
            Assert::AreEqual(0, wrongThread.load(), L"a handler continued off the UI thread");
        }

        TEST_METHOD(TestAddTaskFromAWorkerRunsOnTheUiThread)
        {
            ScopedScheduler workers(SchedulerInfo{.numberOfThreads = 2});
            workers.Start();
            DispatchWindow                  window;
            Schedulers::DispatcherScheduler dispatcher(window.Handle(), WM_DISPATCH);
            window.SetDispatcher(&dispatcher);

            const DWORD       uiThread = GetCurrentThreadId();
            DWORD             ranOn    = 0;
            std::atomic<bool> done     = false;
            auto              uiUpdate = Tasks::Task::Create([&] {
                ranOn = GetCurrentThreadId();
                done.store(true);
            });
            struct ExitFlag : INotifyTaskStateChange {
                std::atomic<bool> exited = false;
                void              OnTaskExited() noexcept override { exited.store(true); }
            } jobExit;
            // fire and forget: a worker finishes and hands a UI update to the dispatcher
            auto job = Tasks::Task::Create([&] { uiUpdate.Schedule(*dispatcher.Get()); }, &jobExit);
            job.Schedule(*workers.Get());

            Assert::IsTrue(DispatchWindow::PumpUntil([&] { return done.load() && jobExit.exited.load(); }), L"the UI update did not run");
            Assert::IsTrue(ranOn == uiThread, L"the UI update ran off the UI thread");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
