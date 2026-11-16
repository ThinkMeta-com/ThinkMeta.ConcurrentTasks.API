// The local scheduler: tasks that run inside Run, on the thread that calls it. First
// two tasks that take turns on the main thread, then the main thread working along
// with the threads of a default scheduler.
#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Schedulers.LocalScheduler;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Tasks.RunAndWait;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Schedulers::LocalScheduler;
using TC::Synchronization::Semaphore_s;
using TC::Tasks::Task;
// [/names]

// [pipeline]
// A producer and a consumer take turns on the calling thread.
int SumOfSquares(int count)
{
    int         value = 0;
    int         sum   = 0;
    Semaphore_s filled;   // the consumer waits for the next value
    Semaphore_s taken{1}; // the producer waits until the value was taken

    auto producer = Task::Create([&] {
        for (int i = 1; i <= count; ++i) {
            taken.Wait();
            value = i * i;
            filled.Release();
        }
    });
    auto consumer = Task::Create([&] {
        for (int i = 1; i <= count; ++i) {
            filled.Wait();
            sum += value;
            taken.Release();
        }
    });

    LocalScheduler scheduler;
    producer.Schedule(scheduler);
    consumer.Schedule(scheduler);
    scheduler.Run(); // runs both tasks here; returns once both have ended
    return sum;
}
// [/pipeline]

constexpr long long Numbers = 40'000'000;

// The sum of one quarter of the numbers 1 to Numbers.
static long long SumOfQuarter(int quarter) noexcept
{
    long long sum = 0;
    for (long long i = quarter * (Numbers / 4) + 1;
         i <= (quarter + 1) * (Numbers / 4); ++i)
        sum += i;
    return sum;
}

// [work-along]
// The calling thread computes one quarter itself; the workers compute the others.
long long SumWithWorkers(TC::ITaskScheduler& workers)
{
    long long sums[4] = {};

    auto handOver = [&](int quarter) {
        // Waits for the worker; the calling thread runs the other local tasks.
        TC::Tasks::RunAndWait(workers,
                              [&] { sums[quarter] = SumOfQuarter(quarter); });
        // Here the task is back on the thread that called Run.
    };
    auto first  = Task::Create([&] { handOver(0); });
    auto second = Task::Create([&] { handOver(1); });
    auto third  = Task::Create([&] { handOver(2); });
    auto own    = Task::Create([&] { sums[3] = SumOfQuarter(3); });

    LocalScheduler scheduler;
    Task*          tasks[] = {&first, &second, &third, &own};
    for (Task* task : tasks)
        task->Schedule(scheduler);
    scheduler.Run(); // returns once all four have ended
    return sums[0] + sums[1] + sums[2] + sums[3];
}
// [/work-along]

int main()
{
    const int  squares   = SumOfSquares(10);
    const bool squaresOk = squares == 385;
    std::printf("producer and consumer on the main thread: %d (%s)\n", squares,
                squaresOk ? "ok" : "MISMATCH");

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* workers
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (workers == nullptr)
        return 1;
    workers->Start();

    const long long sum   = SumWithWorkers(*workers);
    const bool      sumOk = sum == Numbers * (Numbers + 1) / 2;
    std::printf("main thread and workers: %lld (%s)\n", sum,
                sumOk ? "ok" : "MISMATCH");

    workers->Stop();
    workers->Destroy();
    return squaresOk && sumOk ? 0 : 1;
}
