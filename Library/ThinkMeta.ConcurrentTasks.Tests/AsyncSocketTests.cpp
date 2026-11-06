#include "pch.h"

#if defined(_WIN32)
// WinSock2.h must be included before Windows.h (pulled in by TestHelpers.h) to avoid
// the legacy winsock.h redefinition conflicts.
#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    include <WinSock2.h>
#    include <Windows.h>
#    include <ws2ipdef.h>

#    pragma comment(lib, "ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#endif

#include "TestHelpers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;
import ThinkMeta.ConcurrentTasks.Platform.Socket;
import ThinkMeta.ConcurrentTasks.Synchronization.TaskGroup;
import ThinkMeta.ConcurrentTasks.Tasks.Task;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The same tests on Windows (Winsock, completion port) and Linux (io_uring), through the
// platform-neutral names: Platform::SocketError for the error codes (WSA codes or errno).

namespace ThinkMeta::ConcurrentTasks::Tests {
    using Platform::SocketError;

    // RAII helper to initialize/cleanup the sockets (Windows: WSAStartup, loads AcceptEx/ConnectEx/etc.;
    // nothing to do on Linux).
    struct WinsockScope {
        bool ok = false;
        WinsockScope() noexcept { ok = SocketError::Succeeded(Platform::Socket::Startup()); }
        ~WinsockScope() noexcept { Platform::Socket::Cleanup(); }
    };

    // Ends a connection gracefully: DisconnectEx on Windows, shutdown of the sending side elsewhere
    // (Disconnect is Windows only).
    static void EndConnection(AsyncIo::AsyncTcpSocket4& socket) noexcept
    {
#if defined(_WIN32)
        socket.Disconnect();
#else
        socket.Shutdown(SHUT_WR);
#endif
    }

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
            SkipWithoutAsyncIo();
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
                    Assert::IsTrue(SocketError::Succeeded(listener.Create()), L"Listener Create failed");
                    sockaddr_in bindAddr = MakeLoopbackAddress(0);
                    Assert::IsTrue(SocketError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                    Assert::IsTrue(SocketError::Succeeded(listener.Listen()), L"Listener Listen failed");
                    sockaddr_in boundAddr{};
                    Assert::IsTrue(SocketError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                    const short port = ntohs(boundAddr.sin_port);

                    std::atomic<int>    recvError{-1};
                    std::atomic<std::uint32_t> recvBytes{12345};
                    std::atomic<double> waited{0.0};
                    std::atomic<bool>   serverDone{false};
                    std::atomic<bool>   clientDone{false};

                    // The server waits for data that never comes.
                    ActionTask server([&]() {
                        AsyncIo::AsyncTcpSocket4 connection;
                        if (SocketError::Failed(listener.Accept(connection))) {
                            serverDone.store(true);
                            return;
                        }

                        char       buffer[64];
                        std::uint32_t received = 0;
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
                        if (SocketError::Succeeded(socket.Create()) && SocketError::Succeeded(socket.Bind(anyAddr))
                            && SocketError::Succeeded(socket.Connect(serverAddr)))
                            YieldWaitFor([&]() { return serverDone.load(); });
                        clientDone.store(true);
                    });

                    scheduler.AddTask(&server, TaskPriority::Normal);
                    scheduler.AddTask(&client, TaskPriority::Normal);

                    Assert::IsTrue(WaitFor([&]() { return serverDone.load() && clientDone.load(); }), L"server or client did not finish");
                    Assert::AreEqual(static_cast<int>(SocketError::TimedOut), recvError.load(), L"Recv must time out");
                    Assert::AreEqual(std::uint32_t{0}, recvBytes.load(), L"a timed-out Recv receives nothing");
                    Assert::IsTrue(waited.load() >= 50.0, L"Recv timed out early");

                    // before the tasks go: a task must not go before its OnTaskExited
                    scheduler.Stop();
                }

                DisableTimers();
                AsyncIo::DisableAsyncIo();
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestRecvEndsOnCancel)
        {
            SkipWithoutAsyncIo();
            ConvertThreadToTask();
            {
                WinsockScope winsock;
                Assert::IsTrue(winsock.ok, L"WSAStartup failed");
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

                {
                    // no timers: a Recv without a limit ends on a cancel of its task
                    ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                    scheduler.Start();

                    AsyncIo::AsyncTcpSocket4 listener;
                    Assert::IsTrue(SocketError::Succeeded(listener.Create()), L"Listener Create failed");
                    sockaddr_in bindAddr = MakeLoopbackAddress(0);
                    Assert::IsTrue(SocketError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                    Assert::IsTrue(SocketError::Succeeded(listener.Listen()), L"Listener Listen failed");
                    sockaddr_in boundAddr{};
                    Assert::IsTrue(SocketError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                    const short port = ntohs(boundAddr.sin_port);

                    std::atomic<int>   recvError{-1};
                    std::atomic<std::uint32_t> recvBytes{12345};
                    std::atomic<bool>  receiving{false};
                    std::atomic<bool>  serverDone{false};
                    std::atomic<bool>  clientDone{false};

                    // The server waits for data that never comes, until it is cancelled.
                    ActionTask server([&]() {
                        AsyncIo::AsyncTcpSocket4 connection;
                        if (SocketError::Failed(listener.Accept(connection))) {
                            serverDone.store(true);
                            return;
                        }

                        char  buffer[64];
                        std::uint32_t received = 0;
                        receiving.store(true);
                        recvError.store(connection.Recv(buffer, sizeof(buffer), &received));
                        recvBytes.store(received);
                        serverDone.store(true);
                    });

                    // The client connects and stays silent until the server is done.
                    ActionTask client([&]() {
                        AsyncIo::AsyncTcpSocket4 socket;
                        sockaddr_in              anyAddr    = MakeAnyAddress();
                        sockaddr_in              serverAddr = MakeLoopbackAddress(port);
                        if (SocketError::Succeeded(socket.Create()) && SocketError::Succeeded(socket.Bind(anyAddr))
                            && SocketError::Succeeded(socket.Connect(serverAddr)))
                            YieldWaitFor([&]() { return serverDone.load(); });
                        clientDone.store(true);
                    });

                    scheduler.AddTask(&server, TaskPriority::Normal);
                    scheduler.AddTask(&client, TaskPriority::Normal);

                    Assert::IsTrue(WaitFor([&]() { return receiving.load() || serverDone.load(); }), L"the server did not get to its Recv");
                    Sleep(20); // let it suspend
                    Assert::IsTrue(server.Cancel(), L"Cancel failed");

                    Assert::IsTrue(WaitFor([&]() { return serverDone.load() && clientDone.load(); }), L"server or client did not finish");
                    Assert::AreEqual(static_cast<int>(SocketError::OperationAborted), recvError.load(), L"a cancelled Recv must be aborted");
                    Assert::AreEqual(std::uint32_t{0}, recvBytes.load(), L"a cancelled Recv receives nothing");

                    // before the tasks go: a task must not go before its OnTaskExited
                    scheduler.Stop();
                }

                AsyncIo::DisableAsyncIo();
            }
            ConvertTaskToThread();
        }

        TEST_METHOD(TestTcpLoopbackEcho)
        {
            SkipWithoutAsyncIo();
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
                Assert::IsTrue(SocketError::Succeeded(listener.Create()), L"Listener Create failed");

                sockaddr_in bindAddr = MakeLoopbackAddress(0);
                Assert::IsTrue(SocketError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                Assert::IsTrue(SocketError::Succeeded(listener.Listen()), L"Listener Listen failed");

                sockaddr_in boundAddr{};
                Assert::IsTrue(SocketError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                const short port = ntohs(boundAddr.sin_port);
                Assert::IsTrue(port != 0, L"Ephemeral port must be non-zero");

                std::atomic<bool> serverOk{false};
                std::atomic<bool> clientOk{false};

                Synchronization::TaskGroup taskGroup;

                // Server task: accept connection, receive 1 GB in chunks, echo chunks back.
                auto serverTask = std::make_unique<ActionTask>([&listener, &serverOk]() {
                    AsyncIo::AsyncTcpSocket4 acceptSocket;
                    auto err = listener.Accept(acceptSocket);
                    if (SocketError::Failed(err))
                        return;

                    std::vector<char> buffer(ChunkSize);
                    std::size_t totalReceived = 0;

                    while (totalReceived < TotalVolume) {
                        const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalReceived));
                        std::uint32_t received = 0;
                        err = acceptSocket.Recv(buffer.data(), toRead, &received);
                        if (SocketError::Failed(err) || received == 0)
                            return;

                        std::uint32_t sent = 0;
                        err = acceptSocket.Send(buffer.data(), received, &sent);
                        if (SocketError::Failed(err) || sent != received)
                            return;

                        totalReceived += received;
                    }

                    EndConnection(acceptSocket);
                    serverOk.store(true, std::memory_order_release);
                });

                // Client task: connect, stream 1 GB in chunks, receive echo chunks back.
                auto clientTask = std::make_unique<ActionTask>([port, &clientOk]() {
                    AsyncIo::AsyncTcpSocket4 client;
                    if (SocketError::Failed(client.Create()))
                        return;

                    sockaddr_in anyAddr = MakeAnyAddress();
                    if (SocketError::Failed(client.Bind(anyAddr)))
                        return;

                    sockaddr_in serverAddr = MakeLoopbackAddress(port);
                    if (SocketError::Failed(client.Connect(serverAddr)))
                        return;

                    std::vector<char> sendBuf(ChunkSize, 'X');
                    std::vector<char> recvBuf(ChunkSize);
                    std::size_t totalSent = 0;
                    std::size_t totalReceived = 0;

                    while (totalSent < TotalVolume || totalReceived < TotalVolume) {
                        if (totalSent < TotalVolume) {
                            const DWORD toSend = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalSent));
                            std::uint32_t sent = 0;
                            if (SocketError::Succeeded(client.Send(sendBuf.data(), toSend, &sent)) && sent > 0) {
                                totalSent += sent;
                            }
                        }

                        if (totalReceived < TotalVolume) {
                            const DWORD toRecv = static_cast<DWORD>(std::min<std::size_t>(ChunkSize, TotalVolume - totalReceived));
                            std::uint32_t recvd = 0;
                            if (SocketError::Succeeded(client.Recv(recvBuf.data(), toRecv, &recvd)) && recvd > 0) {
                                totalReceived += recvd;
                            }
                        }
                    }

                    if (totalSent == TotalVolume && totalReceived == TotalVolume) {
                        clientOk.store(true, std::memory_order_release);
                    }

                    EndConnection(client);
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
            SkipWithoutAsyncIo();
            for (DWORD numThreads : GetTestThreadCounts()) {
                ConvertThreadToTask();

                WinsockScope winsock;
                Assert::IsTrue(winsock.ok, L"WSAStartup failed");
                Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"Failed to enable Async I/O");

                // The listener and the task outlive the scheduler, and so the task's OnTaskExited. The
                // task only records what it saw; the test thread asserts after the scheduler stopped.
                AsyncIo::AsyncTcpSocket4 listener;
                short port = 0;

                Platform::SocketErrorCode createError = 0, bindError = 0, connectError = 0, acceptError = 0, recvError = 0;
                std::atomic<bool> cancelObserved{false};
                std::atomic<bool> taskDone{false};

                auto recvCancel = [&] {
                    // Establish a connected pair so there is a live socket to receive on.
                    AsyncIo::AsyncTcpSocket4 client;
                    createError = client.Create();
                    if (SocketError::Failed(createError))
                        return;
                    sockaddr_in anyAddr = MakeAnyAddress();
                    bindError = client.Bind(anyAddr);
                    if (SocketError::Failed(bindError))
                        return;

                    AsyncIo::AsyncTcpSocket4 acceptSocket;

                    // Issue the accept asynchronously so we can connect from the same task.
                    sockaddr_in serverAddr = MakeLoopbackAddress(port);
                    connectError = client.Connect(serverAddr);
                    if (SocketError::Failed(connectError))
                        return;
                    acceptError = listener.Accept(acceptSocket);
                    if (SocketError::Failed(acceptError))
                        return;

                    // Post a receive that has no data to satisfy it, then cancel it.
                    std::vector<char> buffer(64, 0);
                    std::uint32_t received = 0;
                    AsyncIo::WaitableAsyncSocketIoRequest recvRequest;

                    recvError = acceptSocket.RecvAsync(buffer.data(), static_cast<std::uint32_t>(buffer.size()), &received, recvRequest);
                    if (!SocketError::Succeeded(recvError) && !SocketError::IsPending(recvError))
                        return;

                    // Cancel the pending receive.
                    acceptSocket.CancelAsyncRequest(recvRequest);

                    // The completion should still fire (with an aborted status); wait for it.
                    recvRequest.Wait();

                    if (SocketError::Failed(recvRequest.GetError()))
                        cancelObserved.store(true, std::memory_order_release);

                    acceptSocket.Close();
                    client.Close();
                };
                auto testTask = Tasks::Task::Create([&recvCancel, &taskDone]() {
                    recvCancel();
                    taskDone.store(true, std::memory_order_release);
                });

                SchedulerInfo schedulerInfo = {.numberOfThreads = numThreads};
                ScopedScheduler scheduler(schedulerInfo);
                scheduler.Start();

                Assert::IsTrue(SocketError::Succeeded(listener.Create()), L"Listener Create failed");

                sockaddr_in bindAddr = MakeLoopbackAddress(0);
                Assert::IsTrue(SocketError::Succeeded(listener.Bind(bindAddr)), L"Listener Bind failed");
                Assert::IsTrue(SocketError::Succeeded(listener.Listen()), L"Listener Listen failed");

                sockaddr_in boundAddr{};
                Assert::IsTrue(SocketError::Succeeded(listener.GetSockName(boundAddr)), L"GetSockName failed");
                port = ntohs(boundAddr.sin_port);

                scheduler.AddTask(&testTask, TaskPriority::Normal);

                const bool finished = WaitFor([&taskDone] { return taskDone.load(std::memory_order_acquire); }, std::chrono::seconds(15));

                listener.Close();
                scheduler.Stop();
                AsyncIo::DisableAsyncIo();
                ConvertTaskToThread();

                Assert::IsTrue(finished, L"TCP recv-cancel test timed out!");
                Assert::IsTrue(SocketError::Succeeded(createError), L"Client Create failed");
                Assert::IsTrue(SocketError::Succeeded(bindError), L"Client Bind failed");
                Assert::IsTrue(SocketError::Succeeded(connectError), L"Client Connect failed");
                Assert::IsTrue(SocketError::Succeeded(acceptError), L"Server Accept failed");
                Assert::IsTrue(SocketError::Succeeded(recvError) || SocketError::IsPending(recvError), L"RecvAsync did not pend");
                Assert::IsTrue(cancelObserved.load(std::memory_order_acquire), L"Cancelled receive should report a failure/aborted status");
            }
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
