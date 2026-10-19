// TCP echo server: a listening socket on 127.0.0.1, one task per accepted
// connection that echoes what it receives, and client tasks in the same program
// that check the echo.
#define NOMINMAX
#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

namespace TC = ThinkMeta::ConcurrentTasks;
using TC::AsyncIo::AsyncTcpSocket4;
using TC::Windows::WinsockError;
using TC::Windows::WinsockErrorCode;

constexpr std::size_t Clients           = 4;
constexpr std::size_t MessagesPerClient = 3;

std::uint32_t MessageSize(std::size_t client, std::size_t message) noexcept
{
    return static_cast<std::uint32_t>(1 + (client * 977 + message * 3001) % 8000);
}

std::byte MessageByte(std::size_t client, std::size_t message,
                      std::size_t i) noexcept
{
    return static_cast<std::byte>(client * 31 + message * 7 + i);
}

// Sends the whole buffer; a send may transfer less than asked.
WinsockErrorCode SendAll(AsyncTcpSocket4& socket, const std::byte* data,
                         std::uint32_t size) noexcept
{
    while (size > 0) {
        DWORD                  sent  = 0;
        const WinsockErrorCode error = socket.Send(data, size, &sent);
        if (WinsockError::Failed(error))
            return error;
        data += sent;
        size -= sent;
    }
    return WinsockError::Success;
}

// [echo]
// One connection: receive, send back, until the client shuts down its side (a
// receive of 0 bytes).
class EchoTask : public TC::Tasks::Task
{
public:
    // moved in by the accept task before the task is started
    AsyncTcpSocket4 socket;
    std::size_t     echoed = 0;

    void Run() noexcept override
    {
        std::byte buffer[4096];
        for (;;) {
            DWORD received = 0;
            if (WinsockError::Failed(socket.Recv(buffer, sizeof(buffer), &received))
                || received == 0)
                break;
            if (WinsockError::Failed(SendAll(socket, buffer, received)))
                break;
            echoed += received;
        }
        socket.Close(); // nothing is pending: Recv and Send have returned
    }
};
// [/echo]

// [accept]
// Accepts a known number of connections and starts one EchoTask for each. The echo
// tasks are owned by main and counted into the group before they are started.
class AcceptTask : public TC::Tasks::Task
{
public:
    AcceptTask(AsyncTcpSocket4& listener, EchoTask* echoTasks,
               TC::Synchronization::TaskGroup_s& group) noexcept
        : listener_(listener),
          echoTasks_(echoTasks),
          group_(group)
    {}

    WinsockErrorCode error = WinsockError::Success;

    void Run() noexcept override
    {
        for (std::size_t i = 0; i < Clients; ++i) {
            AsyncTcpSocket4 connection; // Accept creates it
            error = listener_.Accept(connection);
            if (WinsockError::Failed(error))
                return;
            echoTasks_[i].socket = std::move(connection);
            group_.AddTask(&echoTasks_[i]);
            echoTasks_[i].Schedule(); // on the scheduler of the calling task
        }
    }

private:
    AsyncTcpSocket4&                  listener_;
    EchoTask*                         echoTasks_;
    TC::Synchronization::TaskGroup_s& group_;
};
// [/accept]

// [client]
// Connects, sends each message and reads the echo back, then shuts down its sending
// side and waits until the server has closed the connection.
class ClientTask : public TC::Tasks::Task
{
public:
    std::size_t index = 0;
    sockaddr_in server{};
    bool        ok = false;

    void Run() noexcept override
    {
        AsyncTcpSocket4 socket;
        // ConnectEx needs a bound socket: any port on the loopback address.
        sockaddr_in local{};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (WinsockError::Failed(socket.Create())
            || WinsockError::Failed(socket.Bind(local))
            || WinsockError::Failed(socket.Connect(server)))
            return;

        std::vector<std::byte> message;
        std::vector<std::byte> echo;
        for (std::size_t m = 0; m < MessagesPerClient; ++m) {
            message.resize(MessageSize(index, m));
            for (std::size_t i = 0; i < message.size(); ++i)
                message[i] = MessageByte(index, m, i);
            if (WinsockError::Failed(
                    SendAll(socket, message.data(),
                            static_cast<std::uint32_t>(message.size()))))
                return;

            // TCP is a byte stream: the echo may arrive in several pieces.
            echo.resize(message.size());
            std::size_t received = 0;
            while (received < echo.size()) {
                DWORD bytes = 0;
                if (WinsockError::Failed(socket.Recv(
                        echo.data() + received,
                        static_cast<std::uint32_t>(echo.size() - received), &bytes))
                    || bytes == 0)
                    return;
                received += bytes;
            }
            if (echo != message)
                return;
        }

        // Graceful end: no more data from this side; the server's receive returns 0
        // bytes, it closes, and this side's receive returns 0 bytes too.
        DWORD bytes = 0;
        ok          = WinsockError::Succeeded(socket.Shutdown(SD_SEND))
            && WinsockError::Succeeded(socket.Recv(echo.data(), 1, &bytes))
            && bytes == 0;
    }
};
// [/client]

int main()
{
    TC::ConvertThreadToTask();

    // [startup]
    // Winsock and its extension functions (AcceptEx, ConnectEx, ...), then the
    // core's completion port.
    if (WinsockError::Failed(TC::Windows::Socket::Startup()))
        return 1;
    if (!TC::AsyncIo::EnableAsyncIo())
        return 1;
    // [/startup]

    TC::SchedulerInfo   info{};
    TC::ITaskScheduler* scheduler
        = TC::CreateTaskScheduler(TC::SchedulerKind::Default, &info);
    if (scheduler == nullptr)
        return 1;
    scheduler->Start();

    bool ok = false;
    {
        // [listen]
        // Port 0: the system picks a free port; GetSockName tells which.
        AsyncTcpSocket4 listener;
        sockaddr_in     address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool listening    = WinsockError::Succeeded(listener.Create())
            && WinsockError::Succeeded(listener.Bind(address))
            && WinsockError::Succeeded(listener.Listen())
            && WinsockError::Succeeded(listener.GetSockName(address));
        // [/listen]

        // [objects]
        // All task objects and the group outlive the tasks: they are destroyed
        // after Stop.
        EchoTask   echoTasks[Clients];
        ClientTask clientTasks[Clients];
        // main is the only waiter; the accept task adds to it
        TC::Synchronization::TaskGroup_s group;
        AcceptTask                       acceptTask(listener, echoTasks, group);
        // [/objects]

        if (listening) {
            std::printf("Listening on 127.0.0.1:%u\n", ntohs(address.sin_port));
            // [run]
            group.AddTask(&acceptTask);
            scheduler->AddTask(&acceptTask);
            for (std::size_t i = 0; i < Clients; ++i) {
                clientTasks[i].index  = i;
                clientTasks[i].server = address;
                group.AddTask(&clientTasks[i]);
                scheduler->AddTask(&clientTasks[i]);
            }
            // Accept task, clients and every echo task the accept task started.
            group.Wait();
            listener.Close(); // no Accept is pending any more
            // [/run]

            ok                 = WinsockError::Succeeded(acceptTask.error);
            std::size_t echoed = 0;
            for (std::size_t i = 0; i < Clients; ++i) {
                std::printf("client %zu: %s\n", i,
                            clientTasks[i].ok ? "echo verified" : "FAILED");
                ok = ok && clientTasks[i].ok;
                echoed += echoTasks[i].echoed;
            }
            std::printf("server: %zu connections, %zu bytes echoed\n", Clients,
                        echoed);
        }
        scheduler->Stop();
    }
    scheduler->Destroy();

    // [shutdown]
    // Every socket is closed, no operation is pending.
    TC::AsyncIo::DisableAsyncIo();
    TC::Windows::Socket::Cleanup();
    // [/shutdown]
    TC::ConvertTaskToThread();
    std::printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
