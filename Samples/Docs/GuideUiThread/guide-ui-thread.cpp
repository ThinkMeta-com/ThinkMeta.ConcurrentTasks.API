// Guide "Waiting on a UI thread": an event handler that hands work to a default scheduler and waits
// for it on a dispatcher scheduler - without co_await and without blocking the UI thread.
#include <Windows.h>

#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Schedulers.DispatcherScheduler;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
// [/names]

constexpr UINT WM_DISPATCH = WM_APP + 1; // carries a task that can go on
constexpr int  IdSum       = 1;          // the button

// [parts]
// One part of the work: sums a range of numbers on a worker thread.
class PartTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        sum = 0;
        for (long long n = first; n < end; ++n)
            sum += n;
    }

    long long first = 0;
    long long end   = 0;
    long long sum   = 0;
};
// [/parts]

struct Ui {
    TC::ITaskScheduler*                  workers    = nullptr;
    TC::Schedulers::DispatcherScheduler* dispatcher = nullptr;
    HWND                                 window     = nullptr;
    HWND                                 button     = nullptr;
    bool                                 busy       = false;
    DWORD                                startedOn  = 0;
    DWORD                                endedOn    = 0;
    long long                            total      = 0;
} ui;

// [handler]
// The click handler. It runs as a task of the dispatcher scheduler on the UI thread, so it
// may wait: at the first wait the window procedure returns, and the UI keeps running.
void OnSum() noexcept
{
    ui.busy = true;
    EnableWindow(ui.button, FALSE); // a click during the wait would start the handler again
    ui.startedOn = GetCurrentThreadId();

    constexpr int       Parts = 8;
    constexpr long long Count = 400'000'000;
    PartTask            parts[Parts];
    TC::Synchronization::TaskGroup_s group;
    for (int i = 0; i < Parts; ++i) {
        parts[i].first = Count / Parts * i;
        parts[i].end   = Count / Parts * (i + 1);
        group.AddTask(&parts[i]);
        parts[i].Schedule(*ui.workers); // the workers - not Schedule(), that is the UI thread
    }
    group.Wait(); // suspends the handler, not the thread

    // back on the UI thread
    ui.endedOn = GetCurrentThreadId();
    ui.total   = 0;
    for (const PartTask& part : parts)
        ui.total += part.sum;
    SetWindowTextW(ui.window, L"Done");
    EnableWindow(ui.button, TRUE);
    ui.busy = false;

    DestroyWindow(ui.window); // the sample ends after one click
}

class HandlerTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override { OnSum(); }
} sumHandler; // reused for every click; the disabled button keeps it from running twice
// [/handler]

// [window-procedure]
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IdSum) {
            ui.dispatcher->RunTask(&sumHandler); // returns at the handler's first wait
            return 0;
        }
        break;

    case WM_DISPATCH:
        ui.dispatcher->ContinueTask(lParam); // a waiting handler goes on, on this thread
        return 0;

    case WM_CLOSE:
        if (ui.busy)
            return 0; // keep the window while the handler waits
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
// [/window-procedure]

// [main]
int main()
{
    // The workers: one thread less than the logical processors, so that the UI keeps one.
    const DWORD       processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    TC::SchedulerInfo info{.numberOfThreads = processors > 1 ? processors - 1 : 1};
    ui.workers = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (ui.workers == nullptr)
        return 1;
    ui.workers->Start();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GuideUiThread";
    RegisterClassExW(&wc);
    ui.window = CreateWindowExW(0, wc.lpszClassName, L"Waiting on a UI thread", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 400, 160, nullptr, nullptr, wc.hInstance, nullptr);
    ui.button = CreateWindowExW(0, L"BUTTON", L"Sum", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 20, 100, 30, ui.window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdSum)), wc.hInstance, nullptr);
    ShowWindow(ui.window, SW_SHOWNOACTIVATE);

    {
        // Bound to this thread. A task that can go on is posted to the window as WM_DISPATCH.
        TC::Schedulers::DispatcherScheduler dispatcher(ui.window, WM_DISPATCH);
        ui.dispatcher = &dispatcher;

        PostMessageW(ui.window, WM_COMMAND, IdSum, 0); // click once
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    } // the dispatcher goes before the workers; no handler waits any more

    ui.workers->Stop();
    ui.workers->Destroy();

    constexpr long long Count    = 400'000'000;
    const bool          sumOk    = ui.total == Count * (Count - 1) / 2;
    const bool          threadOk = ui.startedOn == ui.endedOn && ui.startedOn == GetCurrentThreadId();
    std::printf("sum %lld (%s), handler continued on the UI thread: %s\n", ui.total, sumOk ? "ok" : "WRONG", threadOk ? "yes" : "NO");
    return sumOk && threadOk ? 0 : 1;
}
// [/main]
