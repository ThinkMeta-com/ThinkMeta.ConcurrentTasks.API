// Guide "TCP sockets": an echo server with an accept loop and one task per
// connection, and clients that send with SendAsync while they receive with Recv.
// Uses 127.0.0.1 and an ephemeral port; checks its own results outside the regions
// that the guide shows.
#include <WinSock2.h>
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

// [names]
#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Tasks.Task;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncTcpSocket4;
using TC::Windows::WinsockError;
using TC::Windows::WinsockErrorCode;
// [/names]

// [sizes]
constexpr int MaxConnections = 16; // connection tasks the server keeps

// all tasks of the program: server, connections, clients
TC::Synchronization::TaskGroup_s tasks;
// [/sizes]

// [connection]
// One task per connection: echo everything until the peer shuts down its side.
class Connection : public TC::Tasks::Task
{
public:
    AsyncTcpSocket4 socket; // filled by Accept

    void Run() noexcept override
    {
        char buffer[16 * 1024];
        for (;;) {
            DWORD            received = 0;
            WinsockErrorCode error = socket.Recv(buffer, sizeof(buffer), &received);
            if (WinsockError::Failed(error) || received == 0)
                break; // error, or 0 bytes: the peer has shut down sending
            DWORD sent = 0;
            error      = socket.Send(buffer, received, &sent);
            if (WinsockError::Failed(error) || sent != received)
                break;
        }
        socket.Shutdown(SD_SEND); // no more data from us
        socket.Close(); // nothing is pending: both waiting calls have returned
    }
};
// [/connection]

// [server]
// The accept loop: accept, hand the socket to a connection task, accept the next
// one. The server stops when all connection tasks are used or when no client
// connects for half a second.
class Server : public TC::Tasks::Task
{
public:
    AsyncTcpSocket4 listener; // created, bound and listening before the task starts
    Connection      connections[MaxConnections];

    void Run() noexcept override
    {
        for (Connection& connection : connections) {
            const WinsockErrorCode error = listener.Accept(
                connection.socket, nullptr, TC::Duration::Milliseconds(500));
            if (WinsockError::Failed(error))
                break;                  // WinsockError::TimedOut, or another error
            tasks.AddTask(&connection); // a task of the group may add tasks to it
            connection.Schedule();
        }
        listener.Close();
    }
};
// [/server]

// [client]
// Sends a message and receives the echo at the same time: the send runs as an
// overlapped operation with its own request while the task waits in Recv.
bool Echo(const sockaddr_in& server, std::span<const char> message,
          std::span<char> echo) noexcept
{
    AsyncTcpSocket4 socket;
    sockaddr_in     local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // ConnectEx needs a bound socket.
    if (WinsockError::Failed(socket.Create())
        || WinsockError::Failed(socket.Bind(local)))
        return false;
    if (WinsockError::Failed(socket.Connect(server, TC::Duration::Seconds(5))))
        return false;

    TC::AsyncIo::WaitableAsyncSocketIoRequest_s sendRequest;
    DWORD                                       sent  = 0;
    const WinsockErrorCode                      start = socket.SendAsync(
        message.data(), static_cast<std::uint32_t>(message.size()), &sent,
        sendRequest);
    if (WinsockError::Failed(start) && !WinsockError::IsPending(start))
        return false; // not started: never wait for this request

    std::size_t received = 0;
    while (received < echo.size()) {
        DWORD n = 0;
        if (WinsockError::Failed(
                socket.Recv(echo.data() + received,
                            static_cast<std::uint32_t>(echo.size() - received), &n,
                            TC::Duration::Seconds(5)))
            || n == 0)
            break;
        received += n;
    }
    // Giving up: the send must not stay pending.
    if (received < echo.size())
        socket.CancelAllAsyncRequests();
    sendRequest.Wait(); // the request must not go away while the send is pending

    socket.Shutdown(SD_SEND); // the server's Recv returns 0 bytes now
    return WinsockError::Succeeded(sendRequest.GetError())
        && received == echo.size();
}
// [/client]

// Test driver: a client task that sends a message of its own and checks the echo.
constexpr int         Clients     = 8;
constexpr std::size_t MessageSize = 256 * 1024;

// [client-task]
class Client : public TC::Tasks::Task
{
public:
    sockaddr_in server{};
    int         id = 0;
    bool        ok = false;

    void Run() noexcept override
    {
        std::vector<char> message(MessageSize);
        std::vector<char> echo(MessageSize);
        for (std::size_t i = 0; i < MessageSize; ++i)
            message[i] = static_cast<char>(i * (id + 3));
        ok = Echo(server, message, echo) && echo == message;
    }
};
// [/client-task]

int main()
{
    TC::ConvertThreadToTask();
    // [startup]
    // WSAStartup and the extension functions
    if (WinsockError::Failed(TC::Windows::Socket::Startup()))
        return 1;
    if (!TC::AsyncIo::EnableAsyncIo() || !TC::EnableTimers())
        return 1;
    // [/startup]

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    // [listen]
    static Server server; // large: keep it off the stack
    static Client clients[Clients];

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0; // any free port
    if (WinsockError::Failed(server.listener.Create())
        || WinsockError::Failed(server.listener.Bind(address))
        || WinsockError::Failed(server.listener.GetSockName(address))
        || WinsockError::Failed(server.listener.Listen()))
        return 1;
    // [/listen]

    // [start]
    tasks.AddTask(&server);
    scheduler->AddTask(&server);
    for (int i = 0; i < Clients; ++i) {
        clients[i].server = address;
        clients[i].id     = i;
        tasks.AddTask(&clients[i]);
        scheduler->AddTask(&clients[i]);
    }
    tasks.Wait(); // server, connections and clients have exited
    // [/start]

    int good = 0;
    for (const Client& client : clients)
        good += client.ok;
    std::printf("clients with a correct echo: %d of %d\n", good, Clients);
    const bool ok = good == Clients;

    scheduler->Stop();
    scheduler->Destroy();
    // [cleanup]
    TC::DisableTimers();
    TC::AsyncIo::DisableAsyncIo(); // all sockets are closed, nothing is pending
    TC::Windows::Socket::Cleanup();
    // [/cleanup]
    TC::ConvertTaskToThread();
    std::printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}
