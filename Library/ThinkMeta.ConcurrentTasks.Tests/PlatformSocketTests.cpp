#include "pch.h"

#if defined(_WIN32)
// WinSock2.h before Windows.h (pulled in by TestHelpers.h), against the legacy winsock.h
#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    include <WinSock2.h>
#    include <Windows.h>
#    pragma comment(lib, "ws2_32.lib")
#else
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#endif

#include "TestHelpers.h"

#include <cstdint>

#include <ThinkMeta.ConcurrentTasks/Core.h>
import ThinkMeta.ConcurrentTasks.Platform.Socket;
import ThinkMeta.ConcurrentTasks.Net.TcpSocket;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The blocking socket layer through its platform-neutral names, the same on Windows (Winsock) and
// Linux: a TCP connection over the loopback, the addresses on both ends, options and shutdown.
// Nothing asynchronous: the connection is complete once the listener's backlog has it, so no
// accept is needed.

namespace ThinkMeta::ConcurrentTasks::Tests {
    namespace {
        using Platform::SocketError;

        constexpr int ShutdownSend = 1; // SD_SEND, SHUT_WR

        sockaddr_in LoopbackAddress(std::uint16_t port) noexcept
        {
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_port        = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return address;
        }
    } // namespace

    TEST_CLASS(PlatformSocketTests)
    {
    public:
        TEST_METHOD(TestLoopbackConnectAddressesAndOptions)
        {
            Assert::IsTrue(SocketError::Succeeded(Platform::Socket::Startup()), L"Startup failed");

            bool                    listening = false, reuse = false, keepAlive = false;
            sockaddr_in             listenerAddress{}, peerAddress{}, clientAddress{};
            Platform::SocketErrorCode createError = 1, bindError = 1, listenError = 1, nameError = 1, connectError = 1, peerError = 1,
                                      clientNameError = 1, shutdownError = 1, closeError = 1;
            {
                Net::TcpSocket4 listener;
                createError = listener.Create();
                listener.SetOptionReuseAddress(true);
                listener.GetOptionReuseAddress(&reuse);
                bindError   = listener.Bind(LoopbackAddress(0));
                listenError = listener.Listen();
                listener.GetOptionIsListening(&listening);
                nameError = listener.GetSockName(listenerAddress);

                Net::TcpSocket4 client;
                if (SocketError::Succeeded(client.Create())) {
                    connectError = client.Connect(LoopbackAddress(ntohs(listenerAddress.sin_port)));
                    peerError    = client.GetPeerName(peerAddress);
                    clientNameError = client.GetSockName(clientAddress);
                    client.SetOptionKeepAlive(true);
                    client.GetOptionKeepAlive(&keepAlive);
                    shutdownError = client.Shutdown(ShutdownSend);
                    closeError    = client.Close();
                }
            }

            Platform::Socket::Cleanup();

            Assert::IsTrue(SocketError::Succeeded(createError), L"Create failed");
            Assert::IsTrue(reuse, L"SO_REUSEADDR does not read back");
            Assert::IsTrue(SocketError::Succeeded(bindError), L"Bind failed");
            Assert::IsTrue(SocketError::Succeeded(listenError), L"Listen failed");
            Assert::IsTrue(listening, L"the listener does not report listening");
            Assert::IsTrue(SocketError::Succeeded(nameError) && listenerAddress.sin_port != 0, L"GetSockName gave no port");
            Assert::IsTrue(SocketError::Succeeded(connectError), L"Connect failed");
            Assert::IsTrue(SocketError::Succeeded(peerError), L"GetPeerName failed");
            Assert::IsTrue(peerAddress.sin_port == listenerAddress.sin_port, L"the peer is not the listener");
            Assert::IsTrue(SocketError::Succeeded(clientNameError) && clientAddress.sin_port != 0, L"the client has no local port");
            Assert::IsTrue(keepAlive, L"SO_KEEPALIVE does not read back");
            Assert::IsTrue(SocketError::Succeeded(shutdownError), L"Shutdown failed");
            Assert::IsTrue(SocketError::Succeeded(closeError), L"Close failed");
        }
    };
} // namespace ThinkMeta::ConcurrentTasks::Tests
