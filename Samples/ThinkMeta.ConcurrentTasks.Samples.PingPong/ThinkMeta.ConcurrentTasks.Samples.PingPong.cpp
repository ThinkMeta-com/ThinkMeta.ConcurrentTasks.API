#include <Windows.h>

#include <cstddef>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Synchronization.Semaphore;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;

TC::Synchronization::Semaphore_s pingSemaphore(0);
TC::Synchronization::Semaphore_s pongSemaphore(0);

constexpr std::size_t Iterations = 10'000'000;

class PingTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        for (counter_ = 0; counter_ < Iterations; ++counter_) {
            // Signal pong task
            pongSemaphore.Release();
            // Wait for pong task to signal back
            pingSemaphore.Wait();
        }
    }

private:
    std::size_t counter_ = 0;
};

class PongTask : public TC::Tasks::Task
{
public:
    void Run() noexcept override
    {
        for (counter_ = 0; counter_ < Iterations; ++counter_) {
            // Wait for ping task to signal
            pongSemaphore.Wait();
            // Signal back to ping task
            pingSemaphore.Release();
        }
    }

private:
    std::size_t counter_ = 0;
};

PingTask pingTask;
PongTask pongTask;

int main()
{
    TC::ConvertThreadToTask();

    TC::SchedulerInfo schedulerInfo = {.numberOfThreads = 2, .threadCallback = nullptr, .threadCallbackContext = nullptr};

    auto scheduler = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &schedulerInfo);

    scheduler->Start();

    TC::Synchronization::TaskGroup taskGroup;
    taskGroup.AddTask(&pingTask);
    taskGroup.AddTask(&pongTask);

    scheduler->AddTask(&pingTask, TC::TaskPriority::Normal);
    scheduler->AddTask(&pongTask, TC::TaskPriority::Normal);

    taskGroup.Wait();

    scheduler->Stop();
    scheduler->Destroy();

    TC::ConvertTaskToThread();
}
