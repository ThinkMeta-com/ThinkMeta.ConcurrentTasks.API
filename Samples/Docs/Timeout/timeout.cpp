// Timeouts: a wait with a time limit, a sleeping task, a task started later by time
// and a timed start that is cancelled. All of it needs the core's timer thread
// (EnableTimers).
#define NOMINMAX
#include <Windows.h>

#include <chrono>
#include <cstdio>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Duration;
using TC::TaskPriority;
using TC::Tasks::Task;

namespace {
    using Clock = std::chrono::steady_clock;

    long long MillisecondsSince(Clock::time_point start) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()
                                                                     - start)
            .count();
    }
} // namespace

int main()
{
    TC::ConvertThreadToTask();
    // [enable]
    // Without the timer thread a wait with a timeout waits forever and SleepFor
    // returns false at once.
    if (!TC::EnableTimers())
        return 1;
    // [/enable]

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool ok = false;
    {
        // [objects]
        // Only the demo task waits on the event, so the single-waiter variant (with
        // timeouts) fits.
        TC::Synchronization::AutoResetEvent_s event;
        bool                                  setterRan    = false;
        bool                                  cancelledRan = false;
        auto                                  setter       = Task::Create([&] {
            setterRan = true;
            event.Set();
        });
        auto cancelled = Task::Create([&] { cancelledRan = true; });
        // [/objects]

        bool timedOut       = false;
        bool signaled       = false;
        bool slept          = false;
        bool cancelAccepted = false;

        auto demo = Task::Create([&] {
            // [wait-timeout]
            // 1. Nobody sets the event: the wait times out and returns false.
            auto start = Clock::now();
            timedOut   = !event.Wait(Duration::Milliseconds(50));
            std::printf("Wait without Set: %s after %lld ms\n",
                        timedOut ? "timed out" : "signaled",
                        MillisecondsSince(start));
            // [/wait-timeout]

            // [timed-start]
            // 2. A task started by time sets the event after 100 ms; the wait with
            // a 1 s limit returns true as soon as it does. Schedule with a delay
            // must be called from a task and fails only without EnableTimers.
            start = Clock::now();
            if (setter.Schedule(TaskPriority::Default, Duration::Milliseconds(100)))
                signaled = event.Wait(Duration::Seconds(1));
            std::printf("Wait with a timed Set: %s after %lld ms\n",
                        signaled ? "signaled" : "timed out",
                        MillisecondsSince(start));
            // [/timed-start]

            // [sleep]
            // 3. Sleeping suspends only this task; its thread runs other tasks
            // meanwhile.
            start = Clock::now();
            slept = Task::SleepFor(Duration::Milliseconds(20));
            std::printf("SleepFor 20 ms: returned after %lld ms\n",
                        MillisecondsSince(start));
            // [/sleep]

            // [cancel]
            // 4. A start in 10 s that is taken back: the task exits without
            // running.
            cancelAccepted
                = cancelled.Schedule(TaskPriority::Default, Duration::Seconds(10))
                && cancelled.CancelSchedule();
            std::printf("Timed start cancelled: %s\n",
                        cancelAccepted ? "yes" : "no");
            // [/cancel]
        });

        // [join]
        // A cancelled task still reports its exit, so the group counts it out like
        // the others.
        TC::Synchronization::TaskGroup_s group;
        group.AddTask(&demo);
        group.AddTask(&setter);
        group.AddTask(&cancelled);
        scheduler->AddTask(&demo);
        group.Wait();
        // [/join]

        ok = timedOut && signaled && setterRan && slept && cancelAccepted
            && !cancelledRan;
        std::printf("%s\n", ok ? "ok" : "UNEXPECTED RESULT");

        // [shutdown]
        // Timers off while the scheduler still runs: a timed task that has not
        // started yet would exit on its scheduler now.
        TC::DisableTimers();
        scheduler->Stop();
        // [/shutdown]
    }

    scheduler->Destroy();
    TC::ConvertTaskToThread();
    return ok ? 0 : 1;
}
