// Producer/consumer: producer tasks pass work items to consumer tasks through a
// BoundedPointerQueue. Semaphores count the items, because the queue itself never
// waits.
#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Collections.BoundedPointerQueue;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Collections::BoundedPointerQueue;
using TC::Synchronization::Semaphore;

constexpr std::size_t Producers        = 2;
constexpr std::size_t Consumers        = 4;
constexpr std::size_t ItemsPerProducer = 200'000;

struct WorkItem {
    std::uint64_t value = 0;
    bool          stop  = false; // tells a consumer to end
};

// [channel]
// A fixed pool of items circulates between two queues: free items and items with
// work. There are never more than PoolSize items, so a queue with PoolSize slots
// can never overflow - by construction, not by luck. Each queue has a semaphore
// that counts its items.
// a power of two: it is also the queue capacity
constexpr std::size_t PoolSize = 256;

class Channel
{
public:
    bool Initialize() noexcept
    {
        if (!free_.Initialize(PoolSize) || !work_.Initialize(PoolSize))
            return false;
        for (WorkItem& item : pool_)
            free_.Enqueue(&item);
        freeCount_.Release(PoolSize);
        return true;
    }

    WorkItem* AcquireFree() noexcept { return Take(freeCount_, free_); }
    void      Post(WorkItem* item) noexcept { Put(workCount_, work_, item); }
    WorkItem* Receive() noexcept { return Take(workCount_, work_); }
    void      Recycle(WorkItem* item) noexcept { Put(freeCount_, free_, item); }

private:
    static void Put(Semaphore& count, BoundedPointerQueue<WorkItem>& queue,
                    WorkItem* item) noexcept
    {
        queue.Enqueue(item); // first the item, then the permit that announces it
        count.Release();
    }

    static WorkItem* Take(Semaphore&                     count,
                          BoundedPointerQueue<WorkItem>& queue) noexcept
    {
        count.Wait(); // suspends the task until an item is there
        // The permit guarantees an item, but Dequeue can see the queue as empty for
        // a moment while another task takes the head item: just try again.
        WorkItem* item;
        while ((item = queue.Dequeue()) == nullptr)
            YieldProcessor();
        return item;
    }

    std::vector<WorkItem>         pool_ = std::vector<WorkItem>(PoolSize);
    BoundedPointerQueue<WorkItem> free_;
    BoundedPointerQueue<WorkItem> work_;
    // Several tasks wait on each semaphore: the multi-waiter variant.
    Semaphore freeCount_;
    Semaphore workCount_;
};
// [/channel]

// [tasks]
class Producer : public TC::Tasks::Task
{
public:
    Channel*      channel = nullptr;
    std::uint64_t first   = 0; // this producer posts first, first + 1, ...

    void Run() noexcept override
    {
        for (std::uint64_t i = 0; i < ItemsPerProducer; ++i) {
            WorkItem* item = channel->AcquireFree();
            item->value    = first + i;
            item->stop     = false;
            channel->Post(item);
        }
    }
};

class Consumer : public TC::Tasks::Task
{
public:
    Channel*      channel = nullptr;
    std::uint64_t sum     = 0;
    std::uint64_t count   = 0;

    void Run() noexcept override
    {
        for (;;) {
            WorkItem*  item = channel->Receive();
            const bool stop = item->stop;
            if (!stop) {
                sum += item->value;
                ++count;
            }
            channel->Recycle(item);
            if (stop)
                break;
        }
    }
};
// [/tasks]

int main()
{
    TC::ConvertThreadToTask();

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool ok = false;
    {
        // [objects]
        // Everything the tasks use lives in this scope and is destroyed only after
        // Stop.
        Channel  channel;
        Producer producers[Producers];
        Consumer consumers[Consumers];
        // main is the only waiter of each group
        TC::Synchronization::TaskGroup_s producerGroup;
        TC::Synchronization::TaskGroup_s consumerGroup;
        // [/objects]

        if (channel.Initialize()) {
            // [run]
            for (std::size_t i = 0; i < Consumers; ++i) {
                consumers[i].channel = &channel;
                consumerGroup.AddTask(&consumers[i]);
                scheduler->AddTask(&consumers[i]);
            }
            for (std::size_t i = 0; i < Producers; ++i) {
                producers[i].channel = &channel;
                producers[i].first   = i * ItemsPerProducer;
                producerGroup.AddTask(&producers[i]);
                scheduler->AddTask(&producers[i]);
            }

            // Once all work is posted, one stop item per consumer. They come from
            // the same pool, so they cannot overflow the queue either.
            producerGroup.Wait();
            for (std::size_t i = 0; i < Consumers; ++i) {
                WorkItem* item = channel.AcquireFree();
                item->stop     = true;
                channel.Post(item);
            }
            consumerGroup.Wait();
            // [/run]

            std::uint64_t sum   = 0;
            std::uint64_t count = 0;
            for (const Consumer& consumer : consumers) {
                std::printf("consumer: %llu items\n",
                            static_cast<unsigned long long>(consumer.count));
                sum += consumer.sum;
                count += consumer.count;
            }
            const std::uint64_t n = Producers * ItemsPerProducer;
            ok                    = count == n && sum == n * (n - 1) / 2;
            std::printf("%llu items, sum %llu (%s)\n",
                        static_cast<unsigned long long>(count),
                        static_cast<unsigned long long>(sum),
                        ok ? "ok" : "MISMATCH");
        }
        scheduler->Stop();
    }

    scheduler->Destroy();
    TC::ConvertTaskToThread();
    return ok ? 0 : 1;
}
