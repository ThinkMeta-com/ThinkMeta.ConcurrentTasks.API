// Guide "Timeouts and timers": EnableTimers, Wait with a timeout, starting a task
// later, cancelling and advancing a timed start, DisableTimers. Checks its own
// results outside the regions that the guide shows.
#include <Windows.h>

#include <chrono>
#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;
// [/names]
using Clock = std::chrono::steady_clock;

// Test driver state: how often and when the document was saved.
static int               saves = 0;
static Clock::time_point lastSave{};

static void SaveDocument() noexcept
{
    ++saves;
    lastSave = Clock::now();
}

static long long Milliseconds(Clock::duration duration) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// [wait-timeout]
// Waits until the settings are loaded, but at most 200 ms.
bool WaitForSettings(TC::Synchronization::ManualResetEvent_s& loaded) noexcept
{
    if (loaded.Wait(TC::Duration::Milliseconds(200)))
        return true; // signaled in time
    std::printf("loading the settings takes too long\n");
    return false; // timed out: nothing was taken, a Set may still come
}
// [/wait-timeout]

// [reminder]
// A task that saves the document; it can be started by time.
class AutoSave : public TC::Tasks::WaitableTask_s
{
public:
    void Run() noexcept override { SaveDocument(); }
};
// [/reminder]

// [schedule-later]
// Called from a task: start the save in 50 ms on the calling task's scheduler.
static bool SaveLater(AutoSave& save) noexcept
{
    // false: timers not enabled, or the task cannot be started by time
    return save.Schedule(TC::TaskPriority::Normal, TC::Duration::Milliseconds(50));
}
// [/schedule-later]

int main()
{
    TC::ConvertThreadToTask();

    // [enable]
    if (!TC::EnableTimers()) // starts the core's timer thread (counted)
        return 1;
    // [/enable]

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool ok = true;
    // tracks the lambda tasks below; outlives them
    TC::Synchronization::TaskGroup_s helpers;

    {
        // A loader task sets the event after 10 ms: the wait ends early and returns
        // true.
        TC::Synchronization::ManualResetEvent_s loaded;
        auto loader = TC::Tasks::Task::Create([&loaded]() noexcept {
            TC::Tasks::Task::SleepFor(TC::Duration::Milliseconds(10));
            loaded.Set();
        });
        helpers.AddTask(&loader);
        scheduler->AddTask(&loader);
        const bool inTime = WaitForSettings(loaded);
        helpers.Wait(); // the loader has exited before the event goes out of scope
        std::printf("loaded in time:      %s\n", inTime ? "yes" : "no");
        ok = ok && inTime;
    }

    {
        // Nobody sets this event: the wait times out, no earlier than requested (1
        // ms tolerance: whole milliseconds, measured with another clock than the
        // timer's).
        TC::Synchronization::ManualResetEvent_s loaded;
        const auto                              start  = Clock::now();
        const bool                              inTime = WaitForSettings(loaded);
        const auto took = Milliseconds(Clock::now() - start);
        std::printf("timed out:           %s\n",
                    !inTime && took >= 199 ? "yes" : "no");
        ok = ok && !inTime && took >= 199;
    }

    {
        // SaveLater from inside a task; the save starts no earlier than 50 ms
        // later.
        AutoSave  save;
        long long delay   = -1;
        auto      starter = TC::Tasks::Task::Create([&save, &delay]() noexcept {
            const auto start = Clock::now();
            if (!SaveLater(save))
                return;
            save.Wait();
            delay = Milliseconds(lastSave - start);
        });
        helpers.AddTask(&starter);
        scheduler->AddTask(&starter);
        helpers.Wait();
        std::printf("saved later:         %s\n", delay >= 49 ? "yes" : "no");
        ok = ok && delay >= 49 && saves == 1;
    }

    {
        // [cancel]
        // From outside the scheduler: plan the save for ten seconds from now, then
        // take it back.
        AutoSave save;
        bool     cancelled = false;
        if (scheduler->AddTimedTask(&save, TC::TaskPriority::Normal,
                                    TC::Duration::Seconds(10))) {
            cancelled = TC::CancelTimedTask(&save); // same as save.CancelSchedule()
            // Passes either way: a cancelled task exits without running.
            save.Wait();
        }
        // [/cancel]
        std::printf("cancelled:           %s\n",
                    cancelled && saves == 1 ? "yes, not saved" : "no");
        ok = ok && cancelled && saves == 1;
    }

    {
        const auto start = Clock::now();
        // [schedule-now]
        // A save planned for later that is needed now.
        AutoSave save;
        if (scheduler->AddTimedTask(&save, TC::TaskPriority::Normal,
                                    TC::Duration::Seconds(10))) {
            TC::ScheduleTimedTask(&save); // same as save.ScheduleNow()
            save.Wait();
        }
        // [/schedule-now]
        const bool soon = Milliseconds(Clock::now() - start) < 1000;
        std::printf("started now:         %s\n", soon && saves == 2 ? "yes" : "no");
        ok = ok && soon && saves == 2;
    }

    // [disable]
    TC::DisableTimers(); // the last call stops the timer thread
    // [/disable]
    const bool slept = TC::SleepTask(TC::Duration::Milliseconds(100));
    std::printf("sleep after disable: %s\n", slept ? "slept" : "returned false");
    ok = ok && !slept;

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
