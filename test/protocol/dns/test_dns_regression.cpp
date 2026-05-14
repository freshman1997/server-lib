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

void test_packet_rejects_cyclic_compression_pointer()
{
    using namespace yuan::net::dns;
    using namespace yuan::buffer;

    ByteBuffer buffer;
    buffer.append_u16(0x1234);
    buffer.append_u16(0x0100);
    buffer.append_u16(1);
    buffer.append_u16(0);
    buffer.append_u16(0);
    buffer.append_u16(0);

    const uint8_t cyclic_name[] = {0xC0, 0x0C};
    buffer.append(cyclic_name, sizeof(cyclic_name));
    buffer.append_u16(static_cast<uint16_t>(DnsType::A));
    buffer.append_u16(static_cast<uint16_t>(DnsClass::IN));

    DnsPacket parsed;
    require(!parsed.deserialize(buffer), "dns parser should reject cyclic compression pointers");
}

void test_aaaa_and_mx_roundtrip()
{
    using namespace yuan::net::dns;
    using namespace yuan::buffer;

    DnsPacket packet;
    packet.set_session_id(5151);
    packet.set_is_response(true);

    DnsResourceRecord aaaa;
    aaaa.name = "ipv6.local";
    aaaa.type = DnsType::AAAA;
    aaaa.class_ = DnsClass::IN;
    aaaa.ttl = 30;
    aaaa.set_rdata_from_string("2001:db8::1");
    require(!aaaa.rdata.empty(), "dns AAAA should accept compressed ipv6 text");
    packet.add_answer(aaaa);

    DnsResourceRecord mx;
    mx.name = "mail.local";
    mx.type = DnsType::MX;
    mx.class_ = DnsClass::IN;
    mx.ttl = 30;
    mx.set_rdata_from_string("10 mail1.mail.local");
    require(!mx.rdata.empty(), "dns MX should accept preference + exchange format");
    packet.add_answer(mx);

    ByteBuffer buffer;
    require(packet.serialize(buffer), "dns packet with AAAA/MX should serialize");

    DnsPacket parsed;
    require(parsed.deserialize(buffer), "dns packet with AAAA/MX should deserialize");
    require(parsed.get_answers().size() == 2, "dns packet with AAAA/MX should keep answer count");
    require(parsed.get_answers()[0].get_rdata_as_string() == "2001:db8::1",
            "dns AAAA should roundtrip to canonical compressed text");
    require(parsed.get_answers()[1].get_rdata_as_string() == "10 mail1.mail.local",
            "dns MX should roundtrip as preference and exchange");
}

void test_local_client_server_query()
{
    using namespace yuan::net::dns;

    const uint16_t port = reserve_udp_port();
    require(port != 0, "dns regression should reserve a local UDP port");

    DnsServer server;
    server.add_record("regression.local", "127.0.0.42");
    server.add_record("multi.local", "127.0.0.11", DnsType::A);
    server.add_record("multi.local", "node-1", DnsType::TXT);
    server.add_record("ipv6.local", "2001:db8::abcd", DnsType::AAAA);
    server.add_record("mx.local", "20 mail.mx.local", DnsType::MX);
    server.add_record("invalid-ip.local", "999.1.1.1", DnsType::A);
    server.add_record("invalid-v6.local", "2001:::1", DnsType::AAAA);

    require(server.has_record("ipv6.local", DnsType::AAAA, "2001:db8::abcd"),
            "dns server should keep valid AAAA records");
    require(server.has_record("mx.local", DnsType::MX, "20 mail.mx.local"),
            "dns server should keep valid MX records");
    require(!server.has_record("invalid-ip.local", DnsType::A, "999.1.1.1"),
            "dns server should reject invalid A records");
    require(!server.has_record("invalid-v6.local", DnsType::AAAA, "2001:::1"),
            "dns server should reject invalid AAAA records");

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

    require(client.query("ReGrEsSiOn.Local.", DnsType::A, 1000),
            "dns lookup should accept case-insensitive names and trailing dot");
    const auto &normalized = client.get_last_response();
    require(normalized.get_answers().size() == 1,
            "dns normalized lookup should still return one answer");
    require(normalized.get_answers().front().get_rdata_as_string() == "127.0.0.42",
            "dns normalized lookup should return configured address");

    require(client.query("multi.local", DnsType::ANY, 1000),
            "dns ANY query should succeed for names with multiple records");
    const auto &multi = client.get_last_response();
    require(multi.get_answers().size() == 2,
            "dns ANY query should return every record for the name");

    bool has_a = false;
    bool has_txt = false;
    for (const auto &answer : multi.get_answers()) {
        if (answer.type == DnsType::A && answer.get_rdata_as_string() == "127.0.0.11") {
            has_a = true;
        }
        if (answer.type == DnsType::TXT && answer.get_rdata_as_string() == "node-1") {
            has_txt = true;
        }
    }
    require(has_a, "dns ANY response should include A record");
    require(has_txt, "dns ANY response should include TXT record");

    require(client.query("ipv6.local", DnsType::AAAA, 1000),
            "dns AAAA query should succeed for valid ipv6 record");
    const auto &ipv6 = client.get_last_response();
    require(ipv6.get_answers().size() == 1, "dns AAAA lookup should return one answer");
    require(ipv6.get_answers().front().get_rdata_as_string() == "2001:db8::abcd",
            "dns AAAA lookup should return canonical ipv6 text");

    require(client.query("mx.local", DnsType::MX, 1000),
            "dns MX query should succeed for valid MX record");
    const auto &mx = client.get_last_response();
    require(mx.get_answers().size() == 1, "dns MX lookup should return one answer");
    require(mx.get_answers().front().get_rdata_as_string() == "20 mail.mx.local",
            "dns MX lookup should return preference and exchange");

    client.disconnect();
    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

} // namespace

int main()
{
    

    test_packet_roundtrip();
    test_packet_rejects_cyclic_compression_pointer();
    test_aaaa_and_mx_roundtrip();
    test_local_client_server_query();

    std::cout << "dns regression test passed\n";
    return 0;
}
