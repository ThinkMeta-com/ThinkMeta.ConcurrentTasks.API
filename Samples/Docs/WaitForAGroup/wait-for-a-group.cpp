// Wait for a group: fork many short tasks, join them with a TaskGroup, then reuse
// the group for a second round of tasks built from lambdas.
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;

// [sum-task]
// One part of the work: sums a slice of the array. The result stays in the object,
// so the object has to live at least until the group has been waited for.
class SumTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        sum = std::accumulate(part.begin(), part.end(), std::uint64_t{0});
    }

    std::span<const std::uint32_t> part;
    std::uint64_t                  sum = 0;
};
// [/sum-task]

int main()
{
    constexpr std::size_t Values = 16 * 1024 * 1024;
    constexpr std::size_t Parts  = 256;

    std::vector<std::uint32_t> values(Values);
    // Spread over the whole range.
    for (std::size_t i = 0; i < Values; ++i)
        values[i] = static_cast<std::uint32_t>(i * 2654435761u);

    TC::ConvertThreadToTask();

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool ok = true;
    {
        // [fork-join]
        // Declared before the tasks are added and destroyed only after Stop: the
        // group and the task objects must outlive the tasks, including the last
        // exit notification.
        TC::Synchronization::TaskGroup group;
        // not movable: the scheduler holds their addresses
        std::vector<SumTask> tasks(Parts);

        const std::size_t partSize = Values / Parts;
        for (std::size_t i = 0; i < Parts; ++i) {
            tasks[i].part = std::span(values).subspan(i * partSize, partSize);
            group.AddTask(&tasks[i]); // count the task in before it can exit
            scheduler->AddTask(&tasks[i]);
        }
        group.Wait(); // suspends main until every SumTask has exited

        std::uint64_t total = 0;
        for (const SumTask& task : tasks)
            total += task.sum;
        // [/fork-join]

        const std::uint64_t expected
            = std::accumulate(values.begin(), values.end(), std::uint64_t{0});
        ok = ok && total == expected;
        std::printf("Sum of %zu values in %zu tasks: %llu (%s)\n", Values, Parts,
                    static_cast<unsigned long long>(total),
                    total == expected ? "ok" : "MISMATCH");

        // [lambdas]
        // The group is reusable once its count is back at 0. Task::Create returns
        // the LambdaTask by value; the variables keep it alive.
        const auto    half    = values.begin() + Values / 2;
        std::uint32_t lowMax  = 0;
        std::uint32_t highMax = 0;
        auto          lowTask = TC::Tasks::Task::Create(
            [&] { lowMax = *std::max_element(values.begin(), half); });
        auto highTask = TC::Tasks::Task::Create(
            [&] { highMax = *std::max_element(half, values.end()); });

        group.AddTask(&lowTask);
        group.AddTask(&highTask);
        scheduler->AddTask(&lowTask);
        scheduler->AddTask(&highTask);
        group.Wait();

        const std::uint32_t maximum = std::max(lowMax, highMax);
        // [/lambdas]

        const std::uint32_t expectedMax
            = *std::max_element(values.begin(), values.end());
        ok = ok && maximum == expectedMax;
        std::printf("Maximum from 2 tasks: %u (%s)\n", maximum,
                    maximum == expectedMax ? "ok" : "MISMATCH");

        // The tasks and the group are destroyed after this, at the end of the
        // scope.
        scheduler->Stop();
    }

    scheduler->Destroy();
    TC::ConvertTaskToThread();
    return ok ? 0 : 1;
}
