#include "pch.h"

// WinSock2.h must be included before Windows.h (pulled in by TestHelpers.h) to avoid
// the legacy winsock.h redefinition conflicts.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <ws2ipdef.h>

#pragma comment(lib, "ws2_32.lib")

#include "TestHelpers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {

    // RAII helper to initialize/cleanup Winsock (loads AcceptEx/ConnectEx/etc.).
    struct WinsockScope {
        bool ok = false;
        WinsockScope() noexcept { ok = Windows::WinsockError::Succeeded(Windows::Socket::Startup()); }
        ~WinsockScope() noexcept { Windows::Socket::Cleanup(); }
    };

    static sockaddr_in MakeLoopbackAddress(short port) noexcept
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return addr;
    }

    static sockaddr_in MakeAnyAddress() noexcept
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return addr;
    }

    TEST_CLASS(AsyncSocketTests)
    {
    public:
        TEST_METHOD(TestRecvTimesOut)
        {
            ConvertThreadToTask();
            {
                WinsockScope winsock;
                Assert::IsTrue(winsock.ok, L"WSAStartup failed");
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");
                Assert::IsTrue(EnableTimers(), L"Failed to enable timers");

                {
                    ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                    scheduler.Start();

                    AsyncIo::AsyncTcpSocket4 listener;
                    Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Create()), L"Listener Create failed");
                    sockaddr_in bindAddr = MakeLoopbackAddress(0);
                    Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                    Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Listen()), L"Listener Listen failed");
                    sockaddr_in boundAddr{};
                    Assert::IsTrue(Windows::WinsockError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                    const short port = ntohs(boundAddr.sin_port);

                    std::atomic<int>    recvError{-1};
                    std::atomic<DWORD>  recvBytes{12345};
                    std::atomic<double> waited{0.0};
                    std::atomic<bool>   serverDone{false};
                    std::atomic<bool>   clientDone{false};

                    // The server waits for data that never comes.
                    ActionTask server([&]() {
                        AsyncIo::AsyncTcpSocket4 connection;
                        if (Windows::WinsockError::Failed(listener.Accept(connection))) {
                            serverDone.store(true);
                            return;
                        }

                        char       buffer[64];
                        DWORD      received = 0;
                        const auto start    = std::chrono::steady_clock::now();
                        recvError.store(connection.Recv(buffer, sizeof(buffer), &received, Duration::Milliseconds(50)));
                        waited.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                        recvBytes.store(received);
                        serverDone.store(true);
                    });

                    // The client connects and stays silent until the server is done.
                    ActionTask client([&]() {
                        AsyncIo::AsyncTcpSocket4 socket;
                        sockaddr_in              anyAddr    = MakeAnyAddress();
                        sockaddr_in              serverAddr = MakeLoopbackAddress(port);
                        if (Windows::WinsockError::Succeeded(socket.Create()) && Windows::WinsockError::Succeeded(socket.Bind(anyAddr))
                            && Windows::WinsockError::Succeeded(socket.Connect(serverAddr)))
                            YieldWaitFor([&]() { return serverDone.load(); });
                        clientDone.store(true);
                    });

                    scheduler.AddTask(&server, TaskPriority::Normal);
                    scheduler.AddTask(&client, TaskPriority::Normal);

                    Assert::IsTrue(WaitFor([&]() { return serverDone.load() && clientDone.load(); }), L"server or client did not finish");
                    Assert::AreEqual(static_cast<int>(WSAETIMEDOUT), recvError.load(), L"Recv must time out");
                    Assert::AreEqual(DWORD{0}, recvBytes.load(), L"a timed-out Recv receives nothing");
                    Assert::IsTrue(waited.load() >= 50.0, L"Recv timed out early");
                }

                DisableTimers();
                AsyncIo::DisableAsyncIo();
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestTcpLoopbackEcho)
        {
            for (DWORD numThreads : GetMultiThreadTestCounts()) {
                ConvertThreadToTask();

                WinsockScope winsock;
                Assert::IsTrue(winsock.ok, L"WSAStartup failed");
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                // 1 GiB total streaming data volume over TCP loopback
                constexpr std::size_t TotalVolume = 1024ULL * 1024ULL * 1024ULL; // 1 GB
                constexpr DWORD ChunkSize         = 64 * 1024;                    // 64 KB per socket I/O call

                // Set up a listening server socket bound to an ephemeral loopback port.
                AsyncIo::AsyncTcpSocket4 listener;
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Create()), L"Listener Create failed");

                sockaddr_in bindAddr = MakeLoopbackAddress(0);
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Listen()), L"Listener Listen failed");

                sockaddr_in boundAddr{};
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                const short port = ntohs(boundAddr.sin_port);
                Assert::IsTrue(port != 0, L"Ephemeral port must be non-zero");

                std::atomic<bool> serverOk{false};
                std::atomic<bool> clientOk{false};

                Synchronization::TaskGroup taskGroup;

                // Server task: accept connection, receive 1 GB in chunks, echo chunks back.
                auto serverTask = std::make_unique<ActionTask>([&listener, &serverOk, TotalVolume, ChunkSize]() {
                    AsyncIo::AsyncTcpSocket4 acceptSocket;
                    auto err = listener.Accept(acceptSocket);
                    if (Windows::WinsockError::Failed(err))
                        return;

                    std::vector<char> buffer(ChunkSize);
                    std::size_t totalReceived = 0;

                    while (totalReceived < TotalVolume) {
                        const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalReceived));
                        DWORD received = 0;
                        err = acceptSocket.Recv(buffer.data(), toRead, &received);
                        if (Windows::WinsockError::Failed(err) || received == 0)
                            return;

                        DWORD sent = 0;
                        err = acceptSocket.Send(buffer.data(), received, &sent);
                        if (Windows::WinsockError::Failed(err) || sent != received)
                            return;

                        totalReceived += received;
                    }

                    acceptSocket.Disconnect();
                    serverOk.store(true, std::memory_order_release);
                });

                // Client task: connect, stream 1 GB in chunks, receive echo chunks back.
                auto clientTask = std::make_unique<ActionTask>([port, &clientOk, TotalVolume, ChunkSize]() {
                    AsyncIo::AsyncTcpSocket4 client;
                    if (Windows::WinsockError::Failed(client.Create()))
                        return;

                    sockaddr_in anyAddr = MakeAnyAddress();
                    if (Windows::WinsockError::Failed(client.Bind(anyAddr)))
                        return;

                    sockaddr_in serverAddr = MakeLoopbackAddress(port);
                    if (Windows::WinsockError::Failed(client.Connect(serverAddr)))
                        return;

                    std::vector<char> sendBuf(ChunkSize, 'X');
                    std::vector<char> recvBuf(ChunkSize);
                    std::size_t totalSent = 0;
                    std::size_t totalReceived = 0;

                    while (totalSent < TotalVolume || totalReceived < TotalVolume) {
                        if (totalSent < TotalVolume) {
                            const DWORD toSend = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalSent));
                            DWORD sent = 0;
                            if (Windows::WinsockError::Succeeded(client.Send(sendBuf.data(), toSend, &sent)) && sent > 0) {
                                totalSent += sent;
                            }
                        }

                        if (totalReceived < TotalVolume) {
                            const DWORD toRecv = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalReceived));
                            DWORD recvd = 0;
                            if (Windows::WinsockError::Succeeded(client.Recv(recvBuf.data(), toRecv, &recvd)) && recvd > 0) {
                                totalReceived += recvd;
                            }
                        }
                    }

                    if (totalSent == TotalVolume && totalReceived == TotalVolume) {
                        clientOk.store(true, std::memory_order_release);
                    }

                    client.Disconnect();
                });

                taskGroup.AddTask(serverTask.get());
                taskGroup.AddTask(clientTask.get());
                scheduler.AddTask(serverTask.get(), TaskPriority::Normal);
                scheduler.AddTask(clientTask.get(), TaskPriority::Normal);

                const bool finished = WaitFor([&serverOk, &clientOk] {
                    return serverOk.load(std::memory_order_acquire) && clientOk.load(std::memory_order_acquire);
                }, std::chrono::seconds(60));

                taskGroup.Wait();

                Assert::IsTrue(serverOk.load(std::memory_order_acquire), L"Server did not complete 1 GB echo exchange");
                Assert::IsTrue(clientOk.load(std::memory_order_acquire), L"Client did not complete 1 GB echo exchange");
                Assert::IsTrue(finished, L"1 GB TCP loopback echo test timed out!");

                listener.Close();
                AsyncIo::DisableAsyncIo();

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }

        TEST_METHOD(TestTcpRecvCancel)
        {
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                WinsockScope winsock;
                Assert::IsTrue(winsock.ok, L"WSAStartup failed");
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                AsyncIo::AsyncTcpSocket4 listener;
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Create()), L"Listener Create failed");

                sockaddr_in bindAddr = MakeLoopbackAddress(0);
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Listen()), L"Listener Listen failed");

                sockaddr_in boundAddr{};
                Assert::IsTrue(Windows::WinsockError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                const short port = ntohs(boundAddr.sin_port);

                std::atomic<bool> cancelObserved{false};
                std::atomic<bool> taskDone{false};

                auto testTask = Tasks::Task::Create([&listener, port, &cancelObserved, &taskDone]() {
                    // Establish a connected pair so there is a live socket to receive on.
                    AsyncIo::AsyncTcpSocket4 client;
                    Assert::IsTrue(Windows::WinsockError::Succeeded(client.Create()), L"Client Create failed");
                    sockaddr_in anyAddr = MakeAnyAddress();
                    Assert::IsTrue(Windows::WinsockError::Succeeded(client.Bind(anyAddr)), L"Client Bind failed");

                    AsyncIo::AsyncTcpSocket4 acceptSocket;

                    // Issue the accept asynchronously so we can connect from the same task.
                    sockaddr_in serverAddr = MakeLoopbackAddress(port);
                    Assert::IsTrue(Windows::WinsockError::Succeeded(client.Connect(serverAddr)), L"Client Connect failed");
                    Assert::IsTrue(Windows::WinsockError::Succeeded(listener.Accept(acceptSocket)), L"Server Accept failed");

                    // Post a receive that has no data to satisfy it, then cancel it.
                    std::vector<char> buffer(64, 0);
                    DWORD received = 0;
                    AsyncIo::WaitableAsyncSocketIoRequest recvRequest;

                    auto err = acceptSocket.RecvAsync(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &received, recvRequest);
                    Assert::IsTrue(Windows::WinsockError::Succeeded(err) || Windows::WinsockError::IsPending(err), L"RecvAsync did not pend");

                    // Cancel the pending receive.
                    acceptSocket.CancelAsyncRequest(recvRequest);

                    // The completion should still fire (with an aborted status); wait for it.
                    recvRequest.Wait();

                    if (Windows::WinsockError::Failed(recvRequest.GetError()))
                        cancelObserved.store(true, std::memory_order_release);

                    acceptSocket.Close();
                    client.Close();
                    taskDone.store(true, std::memory_order_release);
                });

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); }, std::chrono::seconds(15));
                Assert::IsTrue(finished, L"TCP recv-cancel test timed out!");
                Assert::IsTrue(cancelObserved.load(std::memory_order_acquire), L"Cancelled receive should report a failure/aborted status");

                listener.Close();
                AsyncIo::DisableAsyncIo();

                scheduler.Stop();
                ConvertTaskToThread();
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
