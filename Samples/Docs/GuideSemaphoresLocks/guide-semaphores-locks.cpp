// Guide "Semaphores and locks": Semaphore with SemaphoreLock to limit concurrency,
// Semaphore_s as a counter of items, CriticalSection (recursive) with
// CriticalSectionLock and the spinning guard. Checks its own results outside the
// regions that the guide shows.
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.CriticalSection;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;
using namespace TC::Synchronization;
// [/names]

struct Item {
    int id = 0;
};

struct Order {
    int id       = 0;
    int quantity = 0;
};

// Test driver state: how many downloads run at once, and what the consumer has
// seen.
static std::atomic<int> running{0};
static std::atomic<int> peak{0};
static std::uint64_t    processedQuantity = 0;
static int              processedOrders   = 0;
static bool             inOrder           = true;

static void Transfer(const Item&) noexcept
{
    const int now  = running.fetch_add(1) + 1;
    int       seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
    volatile std::uint64_t x = 0;
    for (std::uint64_t i = 0; i < 200'000; ++i)
        x = x + i;
    running.fetch_sub(1);
}

static void Process(const Order& order) noexcept
{
    inOrder = inOrder && order.id == processedOrders;
    ++processedOrders;
    processedQuantity += order.quantity;
}

// [limit]
// At most three downloads at a time; further callers suspend until a slot is free.
class Downloader
{
public:
    void Download(const Item& item) noexcept
    {
        // Wait: suspends the task until a permit is free
        SemaphoreLock slot(slots_);
        Transfer(item);
    } // ~SemaphoreLock: Release of the permit

private:
    Semaphore slots_{3}; // three permits, any number of waiting tasks
};
// [/limit]

// [items]
// The producer stores orders in a fixed array and releases one permit per order;
// the single consumer takes one permit per order. Permits add up: no signal is
// lost.
struct OrderBook {
    Order       orders[100];
    Semaphore_s ready; // count 0; one waiter: the consumer
};

// Producer side: one task or thread that publishes the orders in index order, so
// that order i is in place when the consumer takes the i-th permit.
void Publish(OrderBook& book, int index, const Order& order) noexcept
{
    book.orders[index] = order;
    book.ready.Release(); // order 'index' may be taken now
}

// Consumer side: the one task that waits.
void ProcessOrders(OrderBook& book, int count) noexcept
{
    for (int i = 0; i < count; ++i) {
        book.ready.Wait(); // suspends until order i is there
        Process(book.orders[i]);
    }
}
// [/items]

// [locked-counter]
// Stock levels shared by many tasks, guarded by a task-owned recursive lock.
struct DeliveryLine {
    int item;
    int quantity;
};

class Stock
{
public:
    void Add(int item, int quantity) noexcept
    {
        CriticalSectionLock lock(section_);
        levels_[item] += quantity;
    }

    // Books a whole delivery at once: holds the lock and calls Add, which enters
    // again.
    void AddDelivery(std::span<const DeliveryLine> lines) noexcept
    {
        CriticalSectionLock lock(section_);
        for (const DeliveryLine& line : lines)
            Add(line.item, line.quantity);
    }

    // A very short section: try up to 1000 times before suspending.
    int Level(int item) noexcept
    {
        SpinCriticalSectionLock lock(section_, 1000);
        return levels_[item];
    }

private:
    CriticalSection section_;
    int             levels_[16]{};
};
// [/locked-counter]

int main()
{
    TC::ConvertThreadToTask();
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    // Twelve tasks download at the same time; at most three may be inside Transfer.
    Downloader downloader;
    struct DownloadTask : TC::Tasks::Task {
        Downloader* downloader = nullptr;
        Item        item;
        void        Run() noexcept override { downloader->Download(item); }
    } downloads[12];
    TaskGroup_s downloadsDone;
    for (DownloadTask& download : downloads) {
        download.downloader = &downloader;
        downloadsDone.AddTask(&download);
        scheduler->AddTask(&download);
    }
    downloadsDone.Wait();
    const bool limited = peak.load() >= 1 && peak.load() <= 3;
    std::printf("downloads:  at most 3 at a time: %s\n", limited ? "yes" : "no");

    // One consumer task, main publishes the orders in groups of ten.
    static OrderBook book;
    auto             consumer
        = TC::Tasks::Task::Create([]() noexcept { ProcessOrders(book, 100); });
    TaskGroup_s consumerDone;
    consumerDone.AddTask(&consumer);
    scheduler->AddTask(&consumer);
    for (int i = 0; i < 100; ++i)
        Publish(book, i, Order{i, i % 7 + 1});
    consumerDone.Wait();
    std::uint64_t expectedQuantity = 0;
    for (int i = 0; i < 100; ++i)
        expectedQuantity += i % 7 + 1;
    const bool ordersOk = processedOrders == 100 && inOrder
        && processedQuantity == expectedQuantity;
    std::printf("orders:     %d processed in order: %s\n", processedOrders,
                ordersOk ? "yes" : "no");

    // Four tasks book deliveries at the same time; each delivery adds 1 to item 3
    // and 2 to item 5.
    Stock stock;
    auto  booker = [&stock]() noexcept {
        const DeliveryLine delivery[] = {{3, 1}, {5, 2}};
        for (int i = 0; i < 10'000; ++i) {
            stock.AddDelivery(delivery);
            (void)stock.Level(3);
        }
    };
    auto b0 = TC::Tasks::Task::Create(booker), b1 = TC::Tasks::Task::Create(booker);
    auto b2 = TC::Tasks::Task::Create(booker), b3 = TC::Tasks::Task::Create(booker);
    TC::Tasks::Task* bookers[] = {&b0, &b1, &b2, &b3};
    TaskGroup_s      bookersDone;
    for (TC::Tasks::Task* task : bookers) {
        bookersDone.AddTask(task);
        scheduler->AddTask(task);
    }
    bookersDone.Wait();
    const bool stockOk = stock.Level(3) == 40'000 && stock.Level(5) == 80'000;
    std::printf("stock:      %d and %d\n", stock.Level(3), stock.Level(5));

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    const bool ok = limited && ordersOk && stockOk;
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
