// Guide "Custom primitives": a single-waiter mailbox written directly on Core.h
// (SuspendTask, SuspendTaskFor, ResumeTask, ISuspendCallback), and a countdown
// latch built on the library's Waitable. Checks its own results.
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <span>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.Waitable;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;
// [/names]

// [mailbox]
// Holds one int (0 means empty) for one waiting task. Put may be called from any
// thread.
class Mailbox : private TC::ISuspendCallback
{
public:
    void Put(int value) noexcept
    {
        // 1. the state, 2. the waiter: whoever takes the handle resumes the task
        value_.store(value, std::memory_order_seq_cst);
        if (auto waiter = waiter_.exchange(nullptr, std::memory_order_acq_rel))
            TC::ResumeTask(waiter);
    }

    // Task or converted thread only; at most one waiter at a time.
    int Take() noexcept
    {
        int value;
        // SuspendTask calls OnSuspended once this task is off the processor.
        while ((value = value_.exchange(0, std::memory_order_acq_rel)) == 0)
            TC::SuspendTask(this);
        return value;
    }

    // Returns false if nothing arrived in time (needs EnableTimers, else no limit).
    bool Take(int& value, TC::Duration timeout) noexcept
    {
        while ((value = value_.exchange(0, std::memory_order_acq_rel)) == 0)
            if (!TC::SuspendTaskFor(this, timeout))
                return false; // OnTimeout took the task back
        return true;
    }

private:
    // Runs on the scheduler thread, outside any task: must not suspend, yield or
    // wait.
    void OnSuspended(TC::SuspendedTask task) noexcept override
    {
        // 1. Publish the waiter. 2. Re-check the state: a Put may have missed the
        // waiter, then whoever takes the handle back resumes the task.
        waiter_.store(task, std::memory_order_seq_cst);
        if (value_.load(std::memory_order_seq_cst) != 0)
            if (auto waiter = waiter_.exchange(nullptr, std::memory_order_acq_rel))
                TC::ResumeTask(waiter);
    }

    // Runs on the timer thread while the task is still suspended: take the handle
    // back, or report that a resumer has it already.
    bool OnTimeout(TC::SuspendedTask task) noexcept override
    {
        TC::SuspendedTask expected = task;
        return waiter_.compare_exchange_strong(expected, nullptr,
                                               std::memory_order_acq_rel);
    }

    std::atomic<int>               value_{0};
    std::atomic<TC::SuspendedTask> waiter_{nullptr};
};
// [/mailbox]

// [latch]
// Opens once CountDown has been called count times; any number of tasks may wait.
// Waitable supplies Wait(): it calls TryWait, suspends if that fails, and re-checks
// after the task has been published (then it calls Resume).
class Latch : public TC::Synchronization::Waitable<
                  TC::Synchronization::SuspensionKind::Multiple, Latch>
{
public:
    explicit Latch(std::size_t count) noexcept : count_(count) {}

    void CountDown() noexcept
    {
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            Resume(); // the last one opens the latch
    }

    // Required by Waitable: non-blocking check of the condition.
    bool TryWait() noexcept { return count_.load(std::memory_order_acquire) == 0; }

    // Required by Waitable: resume the tasks the condition lets pass (all of them).
    void Resume() noexcept
    {
        TC::Synchronization::SuspendableBase<
            TC::Synchronization::SuspensionKind::Multiple>::Resume();
    }

private:
    std::atomic<std::size_t> count_;
};
// [/latch]

// Test driver data: a part of some larger work.
struct Part {
    int id   = 0;
    int size = 0;
};

static void Load(Part& part) noexcept
{
    part.size = 100 + part.id;
}

// [mailbox-use]
// A service task: answers requests until it receives a negative one. Values must
// not be 0, which is the mailbox's "empty".
void Serve(Mailbox& requests, Mailbox& replies) noexcept
{
    for (int request; (request = requests.Take()) > 0;)
        replies.Put(request * 2); // ... compute the answer here ...
}

// Called by one task at a time: asks and waits for the answer.
int Ask(Mailbox& requests, Mailbox& replies, int request) noexcept
{
    requests.Put(request);
    return replies.Take();
}
// [/mailbox-use]

// [latch-use]
// Each worker prepares its part and counts the latch down.
void PreparePart(Latch& prepared, Part& part) noexcept
{
    Load(part);
    prepared.CountDown();
}

// Any number of tasks may wait until every part is prepared.
int TotalSize(Latch& prepared, std::span<const Part> parts) noexcept
{
    prepared.Wait();
    int total = 0;
    for (const Part& part : parts)
        total += part.size;
    return total;
}
// [/latch-use]

int main()
{
    TC::ConvertThreadToTask();
    if (!TC::EnableTimers())
        return 1;
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool                             ok = true;
    TC::Synchronization::TaskGroup_s helpers;

    // A service task and main as the client, 10,000 round trips.
    Mailbox requests;
    Mailbox replies;
    auto    service = TC::Tasks::Task::Create(
        [&requests, &replies]() noexcept { Serve(requests, replies); });
    helpers.AddTask(&service);
    scheduler->AddTask(&service);
    long long sum = 0;
    for (int i = 1; i <= 10'000; ++i)
        sum += Ask(requests, replies, i);
    requests.Put(-1);
    helpers.Wait();
    std::printf("mailbox:  sum %lld\n", sum);
    ok = ok && sum == 2LL * 10'000 * 10'001 / 2;

    int value = 0;
    // nobody puts
    const bool received = replies.Take(value, TC::Duration::Milliseconds(30));
    std::printf("timeout:  %s\n", received ? "received" : "timed out");
    ok = ok && !received;

    // Eight tasks prepare the parts; two tasks and main wait for all of them.
    Part parts[8];
    for (int i = 0; i < 8; ++i)
        parts[i].id = i;
    Latch prepared(8);
    int   totals[2] = {};
    auto  w0        = TC::Tasks::Task::Create(
        [&]() noexcept { totals[0] = TotalSize(prepared, parts); });
    auto w1 = TC::Tasks::Task::Create(
        [&]() noexcept { totals[1] = TotalSize(prepared, parts); });
    TC::Tasks::Task* waiters[] = {&w0, &w1};
    for (TC::Tasks::Task* task : waiters) {
        helpers.AddTask(task);
        scheduler->AddTask(task);
    }
    struct PrepareTask : TC::Tasks::Task {
        Latch* prepared = nullptr;
        Part*  part     = nullptr;
        void   Run() noexcept override { PreparePart(*prepared, *part); }
    } preparers[8];
    for (int i = 0; i < 8; ++i) {
        preparers[i].prepared = &prepared;
        preparers[i].part     = &parts[i];
        helpers.AddTask(&preparers[i]);
        scheduler->AddTask(&preparers[i]);
    }
    const int total = TotalSize(prepared, parts);
    helpers.Wait();
    const int expected = 8 * 100 + 28;
    std::printf("latch:    total %d, seen by all three waiters: %s\n", total,
                totals[0] == total && totals[1] == total ? "yes" : "no");
    ok = ok && total == expected && totals[0] == expected && totals[1] == expected;

    scheduler->Stop();
    scheduler->Destroy();
    TC::DisableTimers();
    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
