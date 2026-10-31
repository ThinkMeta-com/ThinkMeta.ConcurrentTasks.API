// Guide "Cancelling tasks": workers that check for a cancel request, a cancellable
// wait, a socket receive ended on shutdown, tasks that are taken back before they
// start, and a sleep cut short. The first three cases run without timers. Uses
// 127.0.0.1 and an ephemeral port; checks its own results outside the regions that
// the guide shows.
#include <WinSock2.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Tasks.WaitableTask;
import ThinkMeta.ConcurrentTasks.Synchronization.AutoResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.ManualResetEvent;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncTcpSocket4;
using TC::Windows::WinsockError;
using TC::Windows::WinsockErrorCode;
// [/names]
using Clock = std::chrono::steady_clock;

static long long Milliseconds(Clock::duration duration) noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

static const char* Name(TC::WaitStatus status) noexcept
{
    switch (status) {
        case TC::WaitStatus::Signaled: return "Signaled";
        case TC::WaitStatus::TimedOut: return "TimedOut";
        case TC::WaitStatus::Cancelled: return "Cancelled";
    }
    return "?";
}

// One step of made-up work.
static std::uint64_t Step(std::uint64_t value) noexcept
{
    for (int i = 0; i < 1000; ++i)
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
    return value;
}

// [worker]
// A worker with far more work than it gets time for. It asks between two steps
// whether it is to stop, and ends itself.
class Worker : public TC::Tasks::Task
{
public:
    std::uint64_t steps    = 0;
    bool          finished = false;

    void Run() noexcept override
    {
        std::uint64_t value = 0;
        for (std::uint64_t i = 0; i < 1'000'000'000'000; ++i) {
            if (TC::IsCancellationRequested())
                return; // cancelled: leave the rest of the work undone
            value = Step(value);
            ++steps;
        }
        finished = value != 1; // practically never reached
    }
};
// [/worker]

// [shutdown]
// The pool knows its workers: on shutdown it cancels every one of them, then waits
// until all have exited. Only then may the workers go away.
class Pool
{
public:
    static constexpr int Size = 4;
    Worker               workers[Size];

    void Start(TC::ITaskScheduler& scheduler) noexcept
    {
        for (Worker& worker : workers) {
            running_.AddTask(&worker);
            scheduler.AddTask(&worker);
        }
    }

    void Shutdown() noexcept
    {
        for (Worker& worker : workers)
            worker.Cancel(); // the same as TC::CancelTask(&worker)
        running_.Wait();
    }

private:
    TC::Synchronization::TaskGroup_s running_;
};
// [/shutdown]

// [wait]
// Waits for the next request. Wait(Duration::Infinite()) has no time limit, but a
// cancel ends it. Wait() without an argument would wait on: it returns only with
// the event set.
class RequestHandler : public TC::Tasks::WaitableTask_s
{
public:
    TC::Synchronization::AutoResetEvent_s   request;
    TC::Synchronization::ManualResetEvent_s started; // for the test driver
    TC::WaitStatus                          status = TC::WaitStatus::Signaled;

    void Run() noexcept override
    {
        started.Set();
        status = request.Wait(TC::Duration::Infinite());
        if (status == TC::WaitStatus::Cancelled)
            return; // no request came; nothing was taken from the event
        // ... handle the request ...
    }
};
// [/wait]

// Test driver: the same wait with Wait(), which a cancel does not end.
class PlainWaiter : public TC::Tasks::WaitableTask_s
{
public:
    TC::Synchronization::AutoResetEvent_s   request;
    TC::Synchronization::ManualResetEvent_s started;
    std::atomic<bool>                       returned{false};
    bool                                    askedToCancel = false;

    void Run() noexcept override
    {
        started.Set();
        request.Wait();
        returned      = true;
        askedToCancel = TC::IsCancellationRequested();
    }
};

// [receive]
// A client that waits for news from its server. Its Recv has no time limit; a
// cancel ends it with WinsockError::OperationAborted.
class Subscriber : public TC::Tasks::WaitableTask_s
{
public:
    sockaddr_in                             server{};
    TC::Synchronization::ManualResetEvent_s connected; // for the test driver
    WinsockErrorCode                        error = 0;

    void Run() noexcept override
    {
        AsyncTcpSocket4 socket;
        sockaddr_in     local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (WinsockError::Failed(socket.Create())
            || WinsockError::Failed(socket.Bind(local))
            || WinsockError::Failed(socket.Connect(server)))
            return;
        connected.Set();

        char  news[256];
        DWORD received = 0;
        error          = socket.Recv(news, sizeof(news), &received);
        // After an aborted operation a stream socket is fit for closing only.
        socket.Close();
    }
};
// [/receive]

// [not-started]
// A report that is not wanted any more before it has run.
class Report : public TC::Tasks::WaitableTask_s
{
public:
    bool ran = false;

    void Run() noexcept override { ran = true; }
};
// [/not-started]

// [sleep]
// Polls something every ten seconds; a cancel cuts the sleep short.
class Poller : public TC::Tasks::WaitableTask_s
{
public:
    TC::Synchronization::ManualResetEvent_s started; // for the test driver
    int                                     polls = 0;

    void Run() noexcept override
    {
        started.Set();
        for (;;) {
            ++polls; // ... poll ...
            if (!TC::Tasks::Task::SleepFor(TC::Duration::Seconds(10)))
                return; // cancelled (or timers not enabled)
        }
    }
};
// [/sleep]

int main()
{
    TC::ConvertThreadToTask();
    if (WinsockError::Failed(TC::Windows::Socket::Startup())
        || !TC::AsyncIo::EnableAsyncIo())
        return 1;

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();
    bool ok = true;

    {
        // The workers run for a while, then the pool shuts down. None finishes its
        // work.
        static Pool pool;
        pool.Start(*scheduler);
        ::Sleep(50);
        const auto start = Clock::now();
        pool.Shutdown();
        const auto took = Milliseconds(Clock::now() - start);
        bool       none = true;
        for (const Worker& worker : pool.workers)
            none = none && !worker.finished;
        std::printf("workers stopped:        %s (in %lld ms)\n",
                    none ? "yes" : "no", took);
        ok = ok && none && took < 1000;
    }

    {
        // [cancel-wait]
        RequestHandler handler;
        scheduler->AddTask(&handler);
        handler.started.Wait(); // test driver: the handler runs
        handler.Cancel();       // ends the wait, or the next one at once
        handler.Wait();         // the handler has exited
        // [/cancel-wait]
        std::printf("wait without limit:     %s\n", Name(handler.status));
        ok = ok && handler.status == TC::WaitStatus::Cancelled;
    }

    {
        // Wait() keeps waiting after the cancel; the task sees the request when the
        // event is set.
        PlainWaiter waiter;
        scheduler->AddTask(&waiter);
        waiter.started.Wait();
        waiter.Cancel();
        ::Sleep(50);
        const bool stillWaiting = !waiter.returned;
        waiter.request.Set();
        waiter.Wait();
        std::printf("Wait() ends on cancel:  %s\n", stillWaiting ? "no" : "yes");
        ok = ok && stillWaiting && waiter.askedToCancel;
    }

    {
        AsyncTcpSocket4 listener;
        sockaddr_in     address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0; // any free port
        if (WinsockError::Failed(listener.Create())
            || WinsockError::Failed(listener.Bind(address))
            || WinsockError::Failed(listener.GetSockName(address))
            || WinsockError::Failed(listener.Listen()))
            return 1;

        Subscriber subscriber;
        subscriber.server = address;
        scheduler->AddTask(&subscriber);
        AsyncTcpSocket4 peer; // the server side; it never sends
        if (WinsockError::Failed(listener.Accept(peer)))
            return 1;
        subscriber.connected.Wait();
        // [cancel-receive]
        // Shutdown: the subscriber's Recv ends, the subscriber closes its socket.
        subscriber.Cancel();
        subscriber.Wait();
        // [/cancel-receive]
        peer.Close();
        listener.Close();
        const bool aborted = subscriber.error == WinsockError::OperationAborted;
        std::printf("receive on shutdown:    %s\n",
                    aborted ? "OperationAborted" : "other result");
        ok = ok && aborted;
    }

    // From here on with timers: a start by time and a sleep.
    if (!TC::EnableTimers())
        return 1;

    {
        // [cancel-early]
        // Cancelled before it was added: the request stands, and the next run is
        // skipped.
        Report early;
        early.Cancel();
        scheduler->AddTask(&early);
        early.Wait(); // passes: the task exits without running

        // Planned for later, then taken back: it exits without running as well.
        Report later;
        if (scheduler->AddTimedTask(&later, TC::TaskPriority::Normal,
                                    TC::Duration::Seconds(10))) {
            later.Cancel();
            later.Wait();
        }
        // [/cancel-early]
        std::printf("reports run:            %s\n",
                    early.ran || later.ran ? "yes" : "none");
        ok = ok && !early.ran && !later.ran;
    }

    {
        Poller poller;
        scheduler->AddTask(&poller);
        poller.started.Wait();
        const auto start = Clock::now();
        poller.Cancel();
        poller.Wait();
        const auto took = Milliseconds(Clock::now() - start);
        std::printf("sleep cut short:        %s\n",
                    took < 1000 && poller.polls == 1 ? "yes" : "no");
        ok = ok && took < 1000 && poller.polls == 1;
    }

    scheduler->Stop();
    scheduler->Destroy();
    TC::DisableTimers();
    TC::AsyncIo::DisableAsyncIo(); // all sockets are closed, nothing is pending
    TC::Windows::Socket::Cleanup();
    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
