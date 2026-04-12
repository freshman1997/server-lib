#include "dns_client.h"
#include "dns_packet.h"
#include "dns_server.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

bool init_winsock()
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

void cleanup_winsock()
{
#ifdef _WIN32
    WSACleanup();
#endif
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

    sockaddr_in addr{};
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

    sockaddr_in bound{};
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

void test_packet_roundtrip()
{
    using namespace yuan::net::dns;
    using namespace yuan::buffer;

    DnsPacket packet;
    packet.set_session_id(4242);
    packet.set_is_response(true);
    packet.set_authoritative_answer(true);
    packet.set_recursion_available(true);

    DnsQuestion question;
    question.name = "example.local";
    question.type = DnsType::A;
    question.class_ = DnsClass::IN;
    packet.add_question(question);

    DnsResourceRecord answer;
    answer.name = "example.local";
    answer.type = DnsType::A;
    answer.class_ = DnsClass::IN;
    answer.ttl = 60;
    answer.set_rdata_from_string("127.0.0.1");
    packet.add_answer(answer);

    ByteBuffer buffer;
    require(packet.serialize(buffer), "dns packet serialization should succeed");

    DnsPacket parsed;
    require(parsed.deserialize(buffer), "dns packet deserialization should succeed");
    require(parsed.get_session_id() == 4242, "dns packet session id should roundtrip");
    require(parsed.is_response(), "dns packet response flag should roundtrip");
    require(parsed.get_questions().size() == 1, "dns packet question count should roundtrip");
    require(parsed.get_answers().size() == 1, "dns packet answer count should roundtrip");
    require(parsed.get_questions().front().name == "example.local", "dns question name should roundtrip");
    require(parsed.get_answers().front().get_rdata_as_string() == "127.0.0.1",
            "dns answer payload should roundtrip");
}

void test_local_client_server_query()
{
    using namespace yuan::net::dns;

    const uint16_t port = reserve_udp_port();
    require(port != 0, "dns regression should reserve a local UDP port");

    DnsServer server;
    server.add_record("regression.local", "127.0.0.42");

    std::thread server_thread([&]() {
        (void)server.serve(port);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    DnsClient client;
    require(client.connect("127.0.0.1", static_cast<short>(port)),
            "dns client should connect to local regression server");

    require(client.query("regression.local", DnsType::A, 1000),
            "dns local client/server query should complete successfully");

    const auto &response = client.get_last_response();
    require(response.is_response(), "dns local query should produce a response packet");
    require(response.get_response_code() == DnsResponseCode::NO_ERROR,
            "dns local query should complete without response error");
    require(response.get_answers().size() == 1, "dns local query should return one answer");
    require(response.get_answers().front().name == "regression.local",
            "dns local answer should keep the expected name");
    require(response.get_answers().front().get_rdata_as_string() == "127.0.0.42",
            "dns local answer should return the configured IP");

    client.disconnect();
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

} // namespace

int main()
{
    if (!init_winsock()) {
        std::cerr << "winsock init failed\n";
        return 1;
    }

    test_packet_roundtrip();
    test_local_client_server_query();

    cleanup_winsock();
    std::cout << "dns regression test passed\n";
    return 0;
}
