// Guide "Events": AutoResetEvent_s, ManualResetEvent, ManualResetEvent_s, Set from
// a plain thread, coalescing Sets and an event on the waiter's stack. Checks its
// own results.
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <thread>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::Synchronization::AutoResetEvent_s;
using TC::Synchronization::ManualResetEvent;
using TC::Synchronization::ManualResetEvent_s;
// [/names]

// Test driver state: counts the tasks that got through the gate.
static std::atomic<int> started{0};

static void Work() noexcept
{
    started.fetch_add(1);
}

// Stands for a call into a library that delivers its result on a thread of its own.
static int ComputeOnLibraryThread() noexcept
{
    Sleep(20);
    return 7;
}

// [request]
// A request lives on the requester's stack; its event tells the requester that it
// is done.
struct Request {
    int                input  = 0;
    int                output = 0;
    ManualResetEvent_s done;
};
// [/request]

// [auto-reset]
// A worker task that sleeps on an auto-reset event and handles one request per
// signal.
class Worker : public TC::Tasks::WaitableTask_s
{
public:
    void Post(Request* request) noexcept
    {
        request_ = request;
        work_.Set(); // wakes the worker, or leaves the event set for its next Wait
    }

    void Quit() noexcept
    {
        request_ = nullptr;
        work_.Set();
    }

    void Run() noexcept override
    {
        for (;;) {
            work_.Wait(); // takes the signal: the event is reset again
            Request* request = request_;
            if (request == nullptr)
                break;
            request->output = request->input * request->input;
            request->done.Set(); // the last access to the request
        }
    }

private:
    AutoResetEvent_s work_; // one waiter: the worker
    Request*         request_ = nullptr;
};
// [/auto-reset]

// [call]
// Called by a task (or converted thread). The request and its event are destroyed
// when this returns, right after the Wait: allowed for the single-waiter (_s)
// events.
int Square(Worker& worker, int x) noexcept
{
    Request request;
    request.input = x;
    worker.Post(&request);
    request.done.Wait();
    return request.output;
}
// [/call]

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

    // [use-worker]
    // Start the worker, send it requests, let it quit.
    Worker worker;
    scheduler->AddTask(&worker);
    int sum = 0;
    for (int i = 1; i <= 10; ++i)
        sum += Square(worker, i);
    worker.Quit();
    worker.Wait();
    // [/use-worker]
    std::printf("sum of squares: %d\n", sum);
    ok = ok && sum == 385; // 1 + 4 + 9 + ... + 100

    // [coalesce]
    AutoResetEvent_s event;
    event.Set();
    event.Set();
    event.Set();                         // no waiter: three Sets leave one signal
    const bool first  = event.TryWait(); // true: takes the signal
    const bool second = event.TryWait(); // false: nothing left
    // [/coalesce]
    std::printf("coalesced:      %d %d\n", first, second);
    ok = ok && first && !second;

    // [manual-reset]
    // A start gate: every task waits until main opens it.
    ManualResetEvent gate;
    auto             runner = [&gate]() noexcept {
        gate.Wait(); // any number of waiters
        Work();
    };
    auto a = TC::Tasks::Task::Create(runner);
    auto b = TC::Tasks::Task::Create(runner);
    auto c = TC::Tasks::Task::Create(runner);
    auto d = TC::Tasks::Task::Create(runner);

    TC::Synchronization::TaskGroup_s runners;
    TC::Tasks::Task*                 tasks[] = {&a, &b, &c, &d};
    for (TC::Tasks::Task* task : tasks) {
        runners.AddTask(task);
        scheduler->AddTask(task);
    }
    gate.Set(); // resumes all waiting tasks; later Waits pass at once
    runners.Wait();
    // [/manual-reset]
    std::printf("through gate:   %d\n", started.load());
    ok = ok && started.load() == 4 && gate.TryWait();

    // [set-from-thread]
    // Set may come from any thread: here a plain OS thread that is not a task.
    ManualResetEvent_s ready;
    int                result = 0;
    std::thread        thread([&ready, &result] {
        result = ComputeOnLibraryThread();
        ready.Set();
    });
    ready.Wait(); // main is a converted thread, so it may wait
    thread.join();
    // [/set-from-thread]
    std::printf("from thread:    %d\n", result);
    ok = ok && result == 7;

    scheduler->Stop();
    scheduler->Destroy();
    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
