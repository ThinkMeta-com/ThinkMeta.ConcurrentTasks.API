// Linux only (built by CMake, not part of the Visual Studio project): AsyncSocket over the core's
// io_uring service, on its own level below AsyncTcpSocket - accept, connect, receive and send,
// message receive and send, a timed-out receive, a cancelled receive and a send to a closed
// connection. The task records what it saw; the test thread asserts, since a failed assertion
// must not leave a task.

#include "pch.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "TestHelpers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncSocket;
import ThinkMeta.ConcurrentTasks.AsyncIo.AsyncTcpSocket;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ThinkMeta::ConcurrentTasks::Tests {
    namespace {
        using AsyncIo::AsyncSocket;
        using Error     = Posix::Error;
        using ByteCount = AsyncSocket::ByteCount;

        sockaddr_in LoopbackAddress(std::uint16_t port) noexcept
        {
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_port        = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return address;
        }

        // A connected pair over the loopback: listener, the accepted server end and the client end.
        // Task only.
        bool ConnectPair(AsyncSocket& listener, AsyncSocket& server, AsyncSocket& client) noexcept
        {
            sockaddr_in bound{};
            if (Error::Failed(listener.Create(AF_INET, SOCK_STREAM, 0)) || Error::Failed(listener.Bind(LoopbackAddress(0))) || Error::Failed(listener.Listen())
                || Error::Failed(listener.GetSockName(bound)))
                return false;

            // the accept runs while the client connects
            AsyncIo::WaitableAsyncSocketIoRequest_s accept;
            if (!Error::IsPending(listener.AcceptAsync(nullptr, nullptr, accept)))
                return false;

            const bool connected = Error::Succeeded(client.Create(AF_INET, SOCK_STREAM, 0))
                                   && Error::Succeeded(client.Connect(LoopbackAddress(ntohs(bound.sin_port))));
            if (!connected)
                listener.CancelAsyncRequest(accept);
            accept.Wait();
            if (Error::Failed(accept.GetError()))
                return false;
            server.Attach(static_cast<int>(accept.GetTransferredBytes()));
            return connected;
        }

        // runs body as a task and waits for it on the test thread
        template <typename Body>
        bool RunAsTask(Body&& body)
        {
            std::atomic<bool> done{false};
            ActionTask        task([&] {
                body();
                done.store(true, std::memory_order_release);
            });

            {
                // the task outlives the scheduler, and so its OnTaskExited
                ScopedScheduler scheduler(SchedulerInfo{.numberOfThreads = 2});
                scheduler.Start();
                scheduler.AddTask(&task);
                WaitFor([&done] { return done.load(std::memory_order_acquire); }, std::chrono::seconds(30));
                scheduler.Stop();
            }
            return done.load(std::memory_order_acquire);
        }
    } // namespace

    TEST_CLASS(AsyncSocketPosixTests)
    {
    public:
        TEST_METHOD(TestAcceptConnectSendRecvAndMessages)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");

            bool                connected = false;
            Posix::ErrorCode    sendError = 1, recvError = 1, sendMsgError = 1, recvMsgError = 1, closedError = 1;
            ByteCount           sent = 0, received = 0, sentMsg = 0, receivedMsg = 0, closedBytes = 1;
            char                text[16] = {}, message[16] = {};
            int                 messageFlags = -1;

            const bool finished = RunAsTask([&] {
                AsyncSocket listener, server, client;
                connected = ConnectPair(listener, server, client);
                if (!connected)
                    return;

                sendError = client.Send("hello", 5, &sent);
                recvError = server.Recv(text, sizeof(text), &received);

                // two buffers out, one buffer in
                char   first[] = "ab", second[] = "cde";
                iovec  out[2]  = {{first, 2}, {second, 3}};
                msghdr outMsg{};
                outMsg.msg_iov    = out;
                outMsg.msg_iovlen = 2;
                sendMsgError      = server.SendMsg(&outMsg, 0, &sentMsg);

                iovec  in{message, sizeof(message)};
                msghdr inMsg{};
                inMsg.msg_iov    = &in;
                inMsg.msg_iovlen = 1;
                recvMsgError     = client.RecvMsg(&inMsg, &receivedMsg);
                messageFlags     = inMsg.msg_flags;

                // the client goes: the server reads the end of the stream
                client.Close();
                closedError = server.Recv(text + 8, 8, &closedBytes);
            });

            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(connected, L"accept or connect failed");
            Assert::IsTrue(Error::Succeeded(sendError) && sent == 5, L"Send failed");
            Assert::IsTrue(Error::Succeeded(recvError) && received == 5 && std::memcmp(text, "hello", 5) == 0, L"Recv did not get what was sent");
            Assert::IsTrue(Error::Succeeded(sendMsgError) && sentMsg == 5, L"SendMsg failed");
            Assert::IsTrue(Error::Succeeded(recvMsgError) && receivedMsg == 5 && std::memcmp(message, "abcde", 5) == 0, L"RecvMsg did not get the message");
            Assert::AreEqual(0, messageFlags, L"RecvMsg reported result flags");
            Assert::IsTrue(Error::Succeeded(closedError) && closedBytes == 0, L"a closed connection must read as 0 bytes");
        }

        TEST_METHOD(TestRecvTimesOutAndCancelAborts)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            Assert::IsTrue(EnableTimers(), L"EnableTimers failed");

            bool             connected = false;
            Posix::ErrorCode timedOut = 0, cancelError = 1, aborted = 0;
            ByteCount        timedOutBytes = 1;
            std::int64_t     waitedMs      = 0;

            const bool finished = RunAsTask([&] {
                AsyncSocket listener, server, client;
                connected = ConnectPair(listener, server, client);
                if (!connected)
                    return;

                char       buffer[8];
                const auto start = std::chrono::steady_clock::now();
                timedOut         = client.Recv(buffer, sizeof(buffer), &timedOutBytes, Duration::Milliseconds(50));
                waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

                AsyncIo::WaitableAsyncSocketIoRequest_s request;
                if (Error::IsPending(client.RecvAsync(buffer, sizeof(buffer), nullptr, request))) {
                    cancelError = client.CancelAsyncRequest(request);
                    request.Wait();
                    aborted = request.GetError();
                }
            });

            DisableTimers();
            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(connected, L"accept or connect failed");
            Assert::IsTrue(timedOut == Error::TimedOut, L"a receive with nothing to read must time out");
            Assert::AreEqual(ByteCount{0}, timedOutBytes, L"a timed-out receive reports 0 bytes");
            Assert::IsTrue(waitedMs >= 40, L"the receive returned before its timeout");
            Assert::IsTrue(Error::Succeeded(cancelError), L"CancelAsyncRequest failed");
            Assert::IsTrue(aborted == Error::OperationAborted, L"a cancelled receive must end with OperationAborted");
        }

        TEST_METHOD(TestTcpSocketAcceptAndConnect)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");

            // AsyncTcpSocket4 the way the portable code uses it: Create without arguments, the
            // waiting Accept with the peer's address, Connect without Bind
            Posix::ErrorCode         acceptError = 1, connectError = 1, peerError = 1;
            sockaddr_in              peerOfServer{}, clientName{}, peerOfClient{}, bound{};
            bool                     listening = false, acceptedValid = false;
            std::atomic<bool>        clientDone{false};
            AsyncIo::AsyncTcpSocket4 client;

            // the client connects from a task of its own while the main task waits in Accept; it
            // outlives the scheduler (RunAsTask), and so its OnTaskExited
            ActionTask connector([&] {
                if (Error::Succeeded(client.Create()))
                    connectError = client.Connect(LoopbackAddress(ntohs(bound.sin_port)));
                if (Error::Succeeded(connectError))
                    client.GetSockName(clientName);
                clientDone.store(true, std::memory_order_release);
            });

            const bool finished = RunAsTask([&] {
                AsyncIo::AsyncTcpSocket4 listener, server;
                listening = Error::Succeeded(listener.Create()) && Error::Succeeded(listener.Bind(LoopbackAddress(0))) && Error::Succeeded(listener.Listen())
                            && Error::Succeeded(listener.GetSockName(bound));
                if (!listening)
                    return;

                GetTaskScheduler()->AddTask(&connector, TaskPriority::Normal);
                acceptError   = listener.Accept(server, &peerOfServer);
                acceptedValid = server.IsValid();
                if (acceptedValid)
                    peerError = server.GetPeerName(peerOfClient);
                YieldWaitFor([&] { return clientDone.load(std::memory_order_acquire); });
            });

            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(listening, L"Create, Bind, Listen or GetSockName failed");
            Assert::IsTrue(Error::Succeeded(acceptError) && acceptedValid, L"Accept failed");
            Assert::IsTrue(Error::Succeeded(connectError), L"Connect failed");
            Assert::IsTrue(peerOfServer.sin_port == clientName.sin_port && peerOfServer.sin_port != 0, L"Accept did not report the client's address");
            Assert::IsTrue(Error::Succeeded(peerError) && peerOfClient.sin_port == clientName.sin_port, L"GetPeerName on the accepted socket failed");
        }

        TEST_METHOD(TestCancelAllAndCloseEndPendingOperations)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");

            bool             connected = false, closedListener = false;
            Posix::ErrorCode cancelAllError = 1, recvError = 0, acceptError = 0;

            const bool finished = RunAsTask([&] {
                AsyncSocket listener, server, client;
                connected = ConnectPair(listener, server, client);
                if (!connected)
                    return;

                // a receive with nothing to read ends through CancelAllAsyncRequests
                char                                    buffer[8];
                AsyncIo::WaitableAsyncSocketIoRequest_s recv;
                if (Error::IsPending(client.RecvAsync(buffer, sizeof(buffer), nullptr, recv))) {
                    cancelAllError = client.CancelAllAsyncRequests();
                    recv.Wait();
                    recvError = recv.GetError();
                }

                // an accept with nobody connecting ends through Close, as closesocket ends it on Windows
                AsyncIo::WaitableAsyncSocketIoRequest_s accept;
                if (Error::IsPending(listener.AcceptAsync(nullptr, nullptr, accept))) {
                    listener.Close();
                    closedListener = !listener.IsValid();
                    accept.Wait();
                    acceptError = accept.GetError();
                }
            });

            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(connected, L"accept or connect failed");
            Assert::IsTrue(Error::Succeeded(cancelAllError), L"CancelAllAsyncRequests failed");
            Assert::IsTrue(recvError == Error::OperationAborted, L"a receive ended by CancelAllAsyncRequests must report OperationAborted");
            Assert::IsTrue(closedListener, L"Close did not give up the socket");
            Assert::IsTrue(acceptError == Error::OperationAborted, L"an accept ended by Close must report OperationAborted");
        }

        TEST_METHOD(TestAcceptTimesOutAndConnectIsRefused)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");
            Assert::IsTrue(EnableTimers(), L"EnableTimers failed");

            bool             listening = false, acceptedNothing = false;
            Posix::ErrorCode acceptError = 0, connectError = 0;

            const bool finished = RunAsTask([&] {
                sockaddr_in bound{};
                {
                    AsyncSocket listener, accepted;
                    listening = Error::Succeeded(listener.Create(AF_INET, SOCK_STREAM, 0)) && Error::Succeeded(listener.Bind(LoopbackAddress(0)))
                                && Error::Succeeded(listener.Listen()) && Error::Succeeded(listener.GetSockName(bound));
                    if (!listening)
                        return;

                    // nobody connects: the accept times out and leaves the target without a socket
                    acceptError     = listener.Accept(accepted, nullptr, nullptr, Duration::Milliseconds(50));
                    acceptedNothing = !accepted.IsValid();
                }

                // the listener is gone: nothing listens on the port any more
                AsyncSocket client;
                if (Error::Succeeded(client.Create(AF_INET, SOCK_STREAM, 0)))
                    connectError = client.Connect(LoopbackAddress(ntohs(bound.sin_port)));
            });

            DisableTimers();
            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(listening, L"Create, Bind, Listen or GetSockName failed");
            Assert::IsTrue(acceptError == Error::TimedOut, L"an accept nobody connects to must time out");
            Assert::IsTrue(acceptedNothing, L"a timed-out accept must leave the target without a socket");
            Assert::IsTrue(connectError == ECONNREFUSED, L"a connect to a closed port must be refused");
        }

        TEST_METHOD(TestSendToClosedConnectionFailsWithoutSignal)
        {
            SkipWithoutAsyncIo();
            Assert::IsTrue(AsyncIo::EnableAsyncIo(), L"EnableAsyncIo failed");

            bool             connected = false;
            Posix::ErrorCode lastError = 0;

            const bool finished = RunAsTask([&] {
                AsyncSocket listener, server, client;
                connected = ConnectPair(listener, server, client);
                if (!connected)
                    return;

                // the peer is gone: once its reset has arrived, a send fails - without SIGPIPE,
                // which would end the test program. Checked 2026-11-05 on kernel 6.18: io_uring
                // raises none even without MSG_NOSIGNAL, so this test cannot notice a missing flag
                // there; the library sets it for kernels that would.
                server.Close();
                for (int i = 0; i < 1000 && Error::Succeeded(lastError); ++i) {
                    ByteCount sent = 0;
                    lastError      = client.Send("x", 1, &sent);
                }
            });

            AsyncIo::DisableAsyncIo();

            Assert::IsTrue(finished, L"the task did not finish");
            Assert::IsTrue(connected, L"accept or connect failed");
            Assert::IsTrue(lastError == EPIPE || lastError == ECONNRESET, L"a send to a closed connection must fail with EPIPE or ECONNRESET");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
