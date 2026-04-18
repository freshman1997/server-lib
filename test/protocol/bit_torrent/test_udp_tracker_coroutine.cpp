#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"
#include "tracker/udp_tracker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "common/winsock_guard.h"

#ifdef _WIN32
#include <WS2tcpip.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

void require(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

uint16_t reserve_udp_port()
{
#ifdef _WIN32
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return 0;
    }
#else
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return 0;
    }
#endif

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return 0;
    }

    sockaddr_in bound {};
#ifdef _WIN32
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (::getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
        return 0;
    }

#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
    return ntohs(bound.sin_port);
}

void append_u32_be(std::vector<char> &buffer, uint32_t value)
{
    const uint32_t network = htonl(value);
    const auto *ptr = reinterpret_cast<const char *>(&network);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(network));
}

void append_u64_be(std::vector<char> &buffer, uint64_t value)
{
    for (int i = 7; i >= 0; --i) {
        buffer.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
    }
}

void append_u16_be(std::vector<char> &buffer, uint16_t value)
{
    const uint16_t network = htons(value);
    const auto *ptr = reinterpret_cast<const char *>(&network);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(network));
}

void run_mock_udp_tracker(uint16_t port, std::atomic_bool &served_announce)
{
#ifdef _WIN32
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(sock != INVALID_SOCKET, "mock udp tracker should create socket");
#else
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    require(sock >= 0, "mock udp tracker should create socket");
#endif

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    require(::bind(sock, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0,
            "mock udp tracker should bind to local port");

#ifdef _WIN32
    DWORD timeout_ms = 1500;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval timeout {};
    timeout.tv_sec = 1;
    timeout.tv_usec = 500000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    sockaddr_in client_addr {};
#ifdef _WIN32
    int client_len = sizeof(client_addr);
#else
    socklen_t client_len = sizeof(client_addr);
#endif

    std::vector<char> request(256);
    const int connect_bytes = ::recvfrom(
        sock,
        request.data(),
        static_cast<int>(request.size()),
        0,
        reinterpret_cast<sockaddr *>(&client_addr),
        &client_len);
    require(connect_bytes >= 16, "mock udp tracker should receive connect request");

    const uint32_t connect_tid = ntohl(*reinterpret_cast<const uint32_t *>(request.data() + 12));
    std::vector<char> connect_response;
    append_u32_be(connect_response, 0);
    append_u32_be(connect_response, connect_tid);
    append_u64_be(connect_response, 0x1020304050607080ULL);
    require(::sendto(
                sock,
                connect_response.data(),
                static_cast<int>(connect_response.size()),
                0,
                reinterpret_cast<const sockaddr *>(&client_addr),
                client_len) == static_cast<int>(connect_response.size()),
            "mock udp tracker should send connect response");

    const int announce_bytes = ::recvfrom(
        sock,
        request.data(),
        static_cast<int>(request.size()),
        0,
        reinterpret_cast<sockaddr *>(&client_addr),
        &client_len);
    require(announce_bytes >= 98, "mock udp tracker should receive announce request");

    const uint32_t announce_tid = ntohl(*reinterpret_cast<const uint32_t *>(request.data() + 12));
    std::vector<char> announce_response;
    append_u32_be(announce_response, 1);
    append_u32_be(announce_response, announce_tid);
    append_u32_be(announce_response, 120);
    append_u32_be(announce_response, 4);
    append_u32_be(announce_response, 9);
    announce_response.push_back(127);
    announce_response.push_back(0);
    announce_response.push_back(0);
    announce_response.push_back(1);
    append_u16_be(announce_response, 51413);

    require(::sendto(
                sock,
                announce_response.data(),
                static_cast<int>(announce_response.size()),
                0,
                reinterpret_cast<const sockaddr *>(&client_addr),
                client_len) == static_cast<int>(announce_response.size()),
            "mock udp tracker should send announce response");

    served_announce.store(true);

#ifdef _WIN32
    ::closesocket(sock);
#else
    ::close(sock);
#endif
}

void test_udp_tracker_runtime_announce_async()
{
    using namespace yuan::net::bit_torrent;

    const uint16_t port = reserve_udp_port();
    require(port != 0, "udp tracker coroutine regression should reserve a local port");

    std::atomic_bool served_announce {false};
    std::thread tracker_thread([&]() {
        run_mock_udp_tracker(port, served_announce);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TorrentMeta meta;
    meta.info_hash_.assign(20, 0x42);

    yuan::timer::WheelTimerManager timer_manager;
    yuan::net::SelectPoller poller;
    yuan::net::EventLoop loop(&poller, &timer_manager);
    yuan::coroutine::RuntimeView runtime(&loop, &timer_manager);

    UdpTracker tracker;
    const auto response = yuan::coroutine::sync_wait(
        runtime,
        tracker.announce_async(runtime, "127.0.0.1", port, meta, 6881));

    require(!response.is_error, "udp tracker runtime announce_async should succeed");
    require(response.interval_ == 120, "udp tracker runtime announce_async should parse interval");
    require(response.incomplete_ == 4, "udp tracker runtime announce_async should parse incomplete count");
    require(response.complete_ == 9, "udp tracker runtime announce_async should parse complete count");
    require(response.peers_.size() == 1, "udp tracker runtime announce_async should return one peer");
    require(response.peers_.front().ip_ == "127.0.0.1", "udp tracker runtime announce_async should parse peer IP");
    require(response.peers_.front().port_ == 51413, "udp tracker runtime announce_async should parse peer port");

    if (tracker_thread.joinable()) {
        tracker_thread.join();
    }

    require(served_announce.load(), "mock udp tracker should serve announce response");
}

} // namespace

int main()
{
    const test::common::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "winsock init failed\n";
        return 1;
    }

    test_udp_tracker_runtime_announce_async();

    std::cout << "udp tracker coroutine test passed\n";
    return 0;
}
