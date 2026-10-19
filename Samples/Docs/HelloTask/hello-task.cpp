// Hello task: start a scheduler, run one task on it, wait for the task, shut down.
#include <Windows.h>

#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;

namespace TC = ThinkMeta::ConcurrentTasks;
// [/names]

// [task]
// A task is an object with a Run method. WaitableTask adds Wait, so others can wait
// for it.
class HelloTask : public TC::Tasks::WaitableTask
{
public:
    void Run() noexcept override { std::printf("Hello from a task\n"); }
};
// [/task]

// [main]
int main()
{
    // The main thread becomes a task too, so it can wait without blocking a
    // scheduler thread.
    TC::ConvertThreadToTask();

    // One scheduler thread per logical processor (numberOfThreads = 0).
    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    HelloTask task; // must outlive the task: from AddTask until it has exited
    scheduler->AddTask(&task);
    task.Wait(); // suspends main until Run has returned

    scheduler->Stop(); // only after every task has finished
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    return 0;
}
// [/main]
