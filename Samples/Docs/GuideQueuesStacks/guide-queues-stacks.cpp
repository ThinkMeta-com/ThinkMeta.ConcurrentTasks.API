// Guide "Queues and stacks": a bounded multi-producer multi-consumer buffer from
// BoundedPointerQueue and two semaphores, a free list of nodes on a
// ConcurrentStack, BasicStack, and Padded statistics. Checks its own results
// outside the regions that the guide shows.
#include <Windows.h>

#include <cstdint>
#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Collections.BoundedPointerQueue;
import ThinkMeta.ConcurrentTasks.Collections.ConcurrentStack;
import ThinkMeta.ConcurrentTasks.Collections.BasicStack;
import ThinkMeta.ConcurrentTasks.Cpp.Padded;

namespace TC = ThinkMeta::ConcurrentTasks;
using namespace TC::Collections;
using TC::Synchronization::Semaphore;
// [/names]

// [node]
// A job that can sit on a ConcurrentStack: derived from SLIST_ENTRY and aligned to
// 16 bytes (MEMORY_ALLOCATION_ALIGNMENT on x64), as the Windows SList requires.
struct alignas(MEMORY_ALLOCATION_ALIGNMENT) Job : SLIST_ENTRY {
    int orderId = 0;
    int amount  = 0;
};
// [/node]

// [sizes]
constexpr std::size_t Capacity  = 64; // queue capacity, a power of two
constexpr int         Producers = 4;  // tasks that submit jobs
constexpr int         Consumers = 4;  // tasks that handle them
// [/sizes]

// [buffer]
// A bounded buffer for any number of producer and consumer tasks. The queue never
// waits, so two semaphores do the waiting: one counts free slots, one counts jobs.
class Buffer
{
public:
    bool Initialize() noexcept { return queue_.Initialize(Capacity); }

    void Put(Job* job) noexcept
    {
        free_.Wait(); // never more than Capacity jobs in the queue
        queue_.Enqueue(job);
        jobs_.Release();
    }

    Job* Take() noexcept
    {
        jobs_.Wait(); // a job is in the queue
        Job* job;
        while ((job = queue_.Dequeue()) == nullptr)
            YieldProcessor(); // nullptr under contention means "try again"
        free_.Release();
        return job;
    }

private:
    BoundedPointerQueue<Job> queue_;
    Semaphore                free_{Capacity};
    Semaphore                jobs_{0};
};
// [/buffer]

// [padded]
// Statistics per consumer, each on its own cache line: consumers on different
// threads do not slow each other down by writing to the same line (false sharing).
struct ConsumerStats {
    std::uint64_t jobs   = 0;
    std::uint64_t amount = 0;
};
TC::Cpp::Padded<ConsumerStats> stats[Consumers];
// [/padded]

// [workers]
// Producer side: take a node from the free list, fill it, put it into the buffer.
void Submit(Buffer& buffer, ConcurrentStack<Job>& pool, int orderId,
            int amount) noexcept
{
    // never empty: enough nodes for every place a node can be
    Job* job     = pool.Pop();
    job->orderId = orderId;
    job->amount  = amount;
    buffer.Put(job);
}

// Consumer side: take a job, handle it, return the node to the free list.
void HandleNext(Buffer& buffer, ConcurrentStack<Job>& pool,
                ConsumerStats& myStats) noexcept
{
    Job* job = buffer.Take();
    ++myStats.jobs;
    myStats.amount += job->amount; // ... handle the job here ...
    pool.Push(job);                // back to the free list
}
// [/workers]

// Jobs each producer submits and each consumer handles.
constexpr int PerTask = 10'000;

int main()
{
    TC::ConvertThreadToTask();
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    Buffer buffer;
    if (!buffer.Initialize())
        return 1;

    // [free-list]
    // A free list of nodes shared by all tasks: Push and Pop are lock-free and
    // never allocate.
    // queue full, one per producer and consumer
    static Job           nodes[Capacity + Producers + Consumers];
    ConcurrentStack<Job> pool;
    for (Job& node : nodes)
        pool.Push(&node);
    // [/free-list]

    // Four producers and four consumers.
    auto producer = [&buffer, &pool](int id) noexcept {
        for (int i = 0; i < PerTask; ++i)
            Submit(buffer, pool, id * PerTask + i, i % 10 + 1);
    };
    auto consumer = [&buffer, &pool](int id) noexcept {
        for (int i = 0; i < PerTask; ++i)
            HandleNext(buffer, pool, stats[id]);
    };
    auto             p0 = TC::Tasks::Task::Create([&]() noexcept { producer(0); });
    auto             p1 = TC::Tasks::Task::Create([&]() noexcept { producer(1); });
    auto             p2 = TC::Tasks::Task::Create([&]() noexcept { producer(2); });
    auto             p3 = TC::Tasks::Task::Create([&]() noexcept { producer(3); });
    auto             c0 = TC::Tasks::Task::Create([&]() noexcept { consumer(0); });
    auto             c1 = TC::Tasks::Task::Create([&]() noexcept { consumer(1); });
    auto             c2 = TC::Tasks::Task::Create([&]() noexcept { consumer(2); });
    auto             c3 = TC::Tasks::Task::Create([&]() noexcept { consumer(3); });
    TC::Tasks::Task* tasks[] = {&p0, &p1, &p2, &p3, &c0, &c1, &c2, &c3};

    TC::Synchronization::TaskGroup_s done;
    for (TC::Tasks::Task* task : tasks) {
        done.AddTask(task);
        scheduler->AddTask(task);
    }
    done.Wait();

    std::uint64_t jobs   = 0;
    std::uint64_t amount = 0;
    for (auto& consumerStats : stats) {
        jobs += consumerStats.jobs;
        amount += consumerStats.amount;
    }
    // 1 + ... + 10 per ten jobs
    const std::uint64_t expectedAmount
        = std::uint64_t{Producers} * PerTask / 10 * 55;
    const bool jobsOk
        = jobs == std::uint64_t{Producers} * PerTask && amount == expectedAmount;
    std::printf("jobs:       %llu handled, amounts %s\n", jobs,
                amount == expectedAmount ? "match" : "differ");

    const auto before = pool.Size();
    // [basic-stack]
    // One task only: take nodes for a batch onto a local stack, return them when
    // done.
    BasicStack<Job> batch;
    for (int i = 0; i < 8; ++i)
        if (Job* job = pool.Pop())
            batch.Push(job);
    // ... fill and use the batch ...
    while (Job* job = batch.Pop())
        pool.Push(job);
    // [/basic-stack]
    const bool poolOk
        = before == Capacity + Producers + Consumers && pool.Size() == before;
    std::printf("free nodes: %u of %zu\n", static_cast<unsigned>(pool.Size()),
                Capacity + Producers + Consumers);

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    const bool ok = jobsOk && poolOk;
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
