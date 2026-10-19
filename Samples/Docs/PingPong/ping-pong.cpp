// Ping-pong: two tasks hand the processor back and forth through two semaphores and
// count the round trips per second.
#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdio>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Synchronization::Semaphore_s;

constexpr std::size_t RoundTrips = 1'000'000;

// [tasks]
// Each semaphore has exactly one waiter - the task it wakes - so the single-waiter
// variant fits.
class PingTask : public TC::Tasks::Task
{
public:
    PingTask(Semaphore_s& ping, Semaphore_s& pong) noexcept
        : ping_(ping),
          pong_(pong)
    {}

    void Run() noexcept override
    {
        for (std::size_t i = 0; i < RoundTrips; ++i) {
            pong_.Release(); // never suspends
            ping_.Wait();    // suspends until the pong task answers
            ++count;
        }
    }

    std::size_t count = 0;

private:
    Semaphore_s& ping_;
    Semaphore_s& pong_;
};

class PongTask : public TC::Tasks::Task
{
public:
    PongTask(Semaphore_s& ping, Semaphore_s& pong) noexcept
        : ping_(ping),
          pong_(pong)
    {}

    void Run() noexcept override
    {
        for (std::size_t i = 0; i < RoundTrips; ++i) {
            pong_.Wait();
            ping_.Release();
            ++count;
        }
    }

    std::size_t count = 0;

private:
    Semaphore_s& ping_;
    Semaphore_s& pong_;
};
// [/tasks]

int main()
{
    // [run]
    TC::ConvertThreadToTask();

    // Two threads, so that a Release can wake the other task on the other thread.
    // With one thread both tasks share it and every round trip is two plain task
    // switches.
    TC::SchedulerInfo   info{.numberOfThreads = 2};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool ok = false;
    {
        // The semaphores, the tasks and the group outlive the tasks: they are
        // destroyed only after Stop.
        Semaphore_s ping;
        Semaphore_s pong;
        PingTask    pingTask(ping, pong);
        PongTask    pongTask(ping, pong);

        TC::Synchronization::TaskGroup_s group; // main is its only waiter
        // before AddTask: the group must count the task in first
        group.AddTask(&pingTask);
        group.AddTask(&pongTask);

        const auto start = std::chrono::steady_clock::now();
        scheduler->AddTask(&pingTask);
        scheduler->AddTask(&pongTask);
        group.Wait();
        const std::chrono::duration<double> elapsed
            = std::chrono::steady_clock::now() - start;

        scheduler->Stop();

        ok = pingTask.count == RoundTrips && pongTask.count == RoundTrips;
        std::printf("%zu round trips in %.3f s: %.1f million per second (%s)\n",
                    RoundTrips, elapsed.count(),
                    static_cast<double>(RoundTrips) / elapsed.count() / 1e6,
                    ok ? "ok" : "MISMATCH");
    }
    // [/run]

    scheduler->Destroy();
    TC::ConvertTaskToThread();
    return ok ? 0 : 1;
}
