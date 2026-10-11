// Guide "Waiting for tasks": WaitableTask_s, WaitableTask with several waiters,
// TaskGroup_s, Task::Create, Schedule from a task and reusing a group. Checks its
// own results.
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;

// Test driver state: what the workers below have processed.
static std::atomic<int> processed{0};

static void Process(int blockSize) noexcept
{
    processed.fetch_add(blockSize);
}

// [waitable-task]
// Sums the numbers in [begin, end). One other task (or converted thread) waits for
// it.
class SumTask : public TC::Tasks::WaitableTask_s
{
public:
    SumTask(std::uint64_t begin, std::uint64_t end) noexcept
        : begin_(begin),
          end_(end)
    {}

    void Run() noexcept override
    {
        for (auto i = begin_; i < end_; ++i)
            sum_ += i;
    }

    std::uint64_t Sum() const noexcept { return sum_; }

private:
    std::uint64_t begin_;
    std::uint64_t end_;
    std::uint64_t sum_ = 0;
};
// [/waitable-task]

// [range-task]
// A plain Task: its end is observed through the group it belongs to.
class RangeTask : public TC::Tasks::Task
{
public:
    void Set(std::uint64_t begin, std::uint64_t end) noexcept
    {
        begin_ = begin;
        end_   = end;
        sum_   = 0;
    }

    void Run() noexcept override
    {
        for (auto i = begin_; i < end_; ++i)
            sum_ += i;
    }

    std::uint64_t Sum() const noexcept { return sum_; }

private:
    std::uint64_t begin_ = 0;
    std::uint64_t end_   = 0;
    std::uint64_t sum_   = 0;
};
// [/range-task]

// [parent-task]
// A task that forks children onto its own scheduler with Schedule and joins them.
// The children and the group are members, so they live as long as the parent
// object.
class ParentTask : public TC::Tasks::WaitableTask_s
{
public:
    void Run() noexcept override
    {
        constexpr std::uint64_t n = 4'000'000;
        for (std::uint64_t i = 0; i < Children; ++i) {
            children_[i].Set(n * i / Children, n * (i + 1) / Children);
            group_.AddTask(&children_[i]); // count in before scheduling
            children_[i].Schedule(TC::TaskPriority::High);
        }
        group_.Wait(); // this task suspends, its thread runs the children

        for (auto& child : children_)
            total_ += child.Sum();
    }

    std::uint64_t Total() const noexcept { return total_; }

private:
    static constexpr std::uint64_t   Children = 4;
    RangeTask                        children_[Children];
    TC::Synchronization::TaskGroup_s group_;
    std::uint64_t                    total_ = 0;
};
// [/parent-task]

static std::uint64_t Expected(std::uint64_t n) noexcept
{
    return n * (n - 1) / 2;
}

int main()
{
    TC::ConvertThreadToTask();
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool ok = true;

    {
        // [wait-one]
        SumTask task(0, 1'000'000);
        scheduler->AddTask(
            &task);  // from a converted thread: AddTask on the scheduler object
        task.Wait(); // suspends main until Run has returned
        // [/wait-one]
        std::printf("one task:      %llu\n", task.Sum());
        ok = ok && task.Sum() == Expected(1'000'000);
    }

    // [many-waiters]
    // One task loads the settings, three workers wait for them: WaitableTask allows
    // any number of waiters.
    struct LoadSettings : TC::Tasks::WaitableTask {
        void Run() noexcept override { blockSize = 64 * 1024; }
        int  blockSize = 0;
    } settings;

    auto worker = [&settings]() noexcept {
        settings.Wait(); // passes once the settings are loaded
        Process(settings.blockSize);
    };
    auto w1 = TC::Tasks::Task::Create(worker);
    auto w2 = TC::Tasks::Task::Create(worker);
    auto w3 = TC::Tasks::Task::Create(worker);

    TC::Synchronization::TaskGroup_s workers;
    TC::Tasks::Task*                 workerTasks[] = {&w1, &w2, &w3};
    for (TC::Tasks::Task* t : workerTasks) {
        workers.AddTask(t);
        scheduler->AddTask(t);
    }
    scheduler->AddTask(&settings);
    workers.Wait();
    // [/many-waiters]
    std::printf("three workers: %d\n", processed.load());
    ok = ok && processed.load() == 3 * 64 * 1024;

    // [task-group]
    // Fork/join from main: eight tasks, one group.
    constexpr std::uint64_t          n = 8'000'000;
    RangeTask                        parts[8];
    TC::Synchronization::TaskGroup_s group;

    for (std::uint64_t i = 0; i < 8; ++i) {
        parts[i].Set(n * i / 8, n * (i + 1) / 8);
        group.AddTask(&parts[i]); // before the task is added to the scheduler
        scheduler->AddTask(&parts[i]);
    }
    group.Wait(); // passes once all eight have exited

    std::uint64_t total = 0;
    for (auto& part : parts)
        total += part.Sum();
    // [/task-group]
    std::printf("eight tasks:   %llu\n", total);
    ok = ok && total == Expected(n);

    // [reuse-group]
    // The count is back at 0: the same group and the same task objects run a second
    // round.
    for (std::uint64_t i = 0; i < 8; ++i) {
        parts[i].Set(2 * n * i / 8, 2 * n * (i + 1) / 8);
        group.AddTask(&parts[i]);
        scheduler->AddTask(&parts[i]);
    }
    group.Wait();

    total = 0;
    for (auto& part : parts)
        total += part.Sum();
    // [/reuse-group]
    std::printf("second round:  %llu\n", total);
    ok = ok && total == Expected(2 * n) && group.TryWait();

    // [run-parent]
    ParentTask parent;
    scheduler->AddTask(&parent);
    parent.Wait();
    // [/run-parent]
    std::printf("parent/child:  %llu\n", parent.Total());
    ok = ok && parent.Total() == Expected(4'000'000);

    // [shutdown]
    scheduler->Stop(); // every task has exited; objects above still exist
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    // [/shutdown]

    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
