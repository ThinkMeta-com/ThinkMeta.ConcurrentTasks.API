// Two schedulers: one takes requests, the other runs their long computations. A
// request hands its computation to the batch scheduler and waits for it; meanwhile
// the request scheduler is free for the next request.
#include <chrono>
#include <cstdio>
#include <thread>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace TC = ThinkMeta::ConcurrentTasks;
using Clock  = std::chrono::steady_clock;
using namespace std::chrono_literals;

static const Clock::time_point start = Clock::now();

static long long MillisecondsSinceStart() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()
                                                                 - start)
        .count();
}

// [tasks]
// A long computation: keeps its thread busy for 50 ms.
class Computation final : public TC::Tasks::WaitableTask_s
{
public:
    void Run() noexcept override
    {
        const auto end = Clock::now() + 50ms;
        while (Clock::now() < end) {
            // ... compute ...
        }
    }
};

// A request: runs on the request scheduler and hands its computation to the
// batch scheduler.
class Request final : public TC::Tasks::Task
{
public:
    Request(int number, TC::ITaskScheduler& batchScheduler) noexcept
        : number_(number),
          batchScheduler_(batchScheduler)
    {}

    void Run() noexcept override
    {
        std::printf("%4lld ms  request %d started\n", MillisecondsSinceStart(),
                    number_);

        // While this task waits, the request thread takes the next request.
        Computation computation;
        computation.Schedule(batchScheduler_);
        computation.Wait();

        // Back on the request scheduler.
        std::printf("%4lld ms  request %d done\n", MillisecondsSinceStart(),
                    number_);
    }

private:
    int                 number_;
    TC::ITaskScheduler& batchScheduler_;
};
// [/tasks]

int main()
{
    TC::ConvertThreadToTask();

    // [schedulers]
    // One thread for requests, two threads for computations.
    TC::SchedulerInfo   requestInfo{.numberOfThreads = 1};
    TC::SchedulerInfo   batchInfo{.numberOfThreads = 2};
    TC::ITaskScheduler* requestScheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &requestInfo);
    TC::ITaskScheduler* batchScheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &batchInfo);
    if (requestScheduler == nullptr || batchScheduler == nullptr)
        return 1;
    requestScheduler->Start();
    batchScheduler->Start();
    // [/schedulers]

    {
        // [send]
        Request requests[] = {{1, *batchScheduler},
                              {2, *batchScheduler},
                              {3, *batchScheduler},
                              {4, *batchScheduler}};

        TC::Synchronization::TaskGroup_s group; // main is its only waiter
        for (Request& request : requests)
            group.AddTask(&request);

        // A request arrives every 10 ms (main is no scheduler thread, it may
        // block).
        for (Request& request : requests) {
            requestScheduler->AddTask(&request);
            std::this_thread::sleep_for(10ms);
        }
        group.Wait();
        // [/send]
    }

    requestScheduler->Stop();
    batchScheduler->Stop();
    requestScheduler->Destroy();
    batchScheduler->Destroy();
    TC::ConvertTaskToThread();
    return 0;
}
