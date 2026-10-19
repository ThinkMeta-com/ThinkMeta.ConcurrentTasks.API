// Guide "Thread callbacks": naming scheduler threads, choosing their ideal
// processor, per-thread state set up on Starting and collected on Exiting, and a
// second scheduler whose single thread joins MMCSS for real-time work. Checks its
// own results outside the regions the guide shows.
#include <Windows.h>
#include <avrt.h>

#include <atomic>
#include <cstdio>
#include <cwchar>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;
// [/names]

// [context]
// What the callback needs from the application. Passed as threadCallbackContext; it
// must outlive the scheduler's threads.
struct ThreadSetup {
    const wchar_t*        name; // prefix of the thread names
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> exited{0};
    // Collected from the threads on Exiting.
    std::atomic<unsigned long> itemsDone{0};
};
// [/context]

// [per-thread]
// Per-thread state: valid on one thread only. A task may continue on another thread
// after it has waited, so code uses it only between two waits.
thread_local unsigned long t_itemsDone = 0;
// [/per-thread]

// [callback]
// Runs on each scheduler thread itself: once before its first task, once after
// Stop. It is plain OS-thread code - no waiting, no synchronization primitives, no
// task functions.
static void SetUpThread(TC::ITaskScheduler*, std::uint32_t threadIndex, HANDLE thread,
                        void* context, TC::ThreadCallbackReason reason) noexcept
{
    auto& setup = *static_cast<ThreadSetup*>(context);

    if (reason == TC::ThreadCallbackReason::Starting) {
        // A name shows up in the debugger, in profilers and in crash dumps.
        wchar_t name[64];
        std::swprintf(name, 64, L"%ls #%lu", setup.name, threadIndex);
        SetThreadDescription(thread, name);

        // Spread the threads over the processors of the current group.
        PROCESSOR_NUMBER ideal{};
        ideal.Number = static_cast<BYTE>(threadIndex % GetActiveProcessorCount(0));
        SetThreadIdealProcessorEx(thread, &ideal, nullptr);

        t_itemsDone = 0;
        setup.started.fetch_add(1);
    } else { // ThreadCallbackReason::Exiting
        setup.itemsDone.fetch_add(t_itemsDone);
        setup.exited.fetch_add(1);
    }
}
// [/callback]

// [mmcss]
// A thread for real-time work, for example live audio: MMCSS raises its priority
// while the thread exists. The handle from AvSetMmThreadCharacteristics must be
// reverted on the same thread, so it is kept in thread-local state between Starting
// and Exiting.
thread_local HANDLE t_mmcss = nullptr;

static void JoinMmcss(TC::ITaskScheduler*, std::uint32_t, HANDLE thread, void*,
                      TC::ThreadCallbackReason reason) noexcept
{
    if (reason == TC::ThreadCallbackReason::Starting) {
        SetThreadDescription(thread, L"audio");
        DWORD taskIndex = 0;
        t_mmcss         = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    } else if (t_mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(t_mmcss);
        t_mmcss = nullptr;
    }
}
// [/mmcss]

// [worker]
// A task that processes some items and counts them in the per-thread state: between
// two waits it stays on one thread.
class Worker : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        for (int i = 0; i < 1000; ++i)
            ++t_itemsDone; // ... process an item ...
    }
};
// [/worker]

int main()
{
    TC::ConvertThreadToTask();
    bool ok = true;

    {
        // [start]
        ThreadSetup setup{.name = L"compute"};

        TC::SchedulerInfo info{};
        info.threadCallback        = SetUpThread;
        info.threadCallbackContext = &setup;
        TC::ITaskScheduler* scheduler
            = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
        // [/start]
        if (scheduler == nullptr)
            return 1;
        scheduler->Start();

        Worker                         workers[64];
        TC::Synchronization::TaskGroup group;
        for (Worker& worker : workers) {
            group.AddTask(&worker);
            scheduler->AddTask(&worker);
        }
        group.Wait();

        // [stop]
        // every thread has run its Exiting callback when Stop returns
        scheduler->Stop();
        scheduler->Destroy();
        std::printf("threads: %u started, %u exited; items: %lu\n",
                    setup.started.load(), setup.exited.load(),
                    setup.itemsDone.load());
        // [/stop]
        ok = ok && setup.started == setup.exited && setup.started > 0
            && setup.itemsDone == 64 * 1000;
    }

    {
        // [audio]
        // One thread for real-time work.
        TC::SchedulerInfo info{};
        info.numberOfThreads = 1;
        info.threadCallback  = JoinMmcss;
        TC::ITaskScheduler* audio
            = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
        // [/audio]
        if (audio == nullptr)
            return 1;
        audio->Start();

        bool joined = false;
        auto probe  = TC::Tasks::Task::Create(
            [&joined]() noexcept { joined = t_mmcss != nullptr; });
        TC::Synchronization::TaskGroup group;
        group.AddTask(&probe);
        audio->AddTask(&probe);
        group.Wait();

        audio->Stop();
        audio->Destroy();
        std::printf("audio thread in MMCSS: %s\n",
                    joined ? "yes" : "no (service not available)");
    }

    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
