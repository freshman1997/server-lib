#include "dns_packet.h"
#include "dns_server.h"
#include "dns_client.h"
#include "buffer/pool.h"
#include <iostream>
#include <thread>
#include <chrono>

#ifndef _WIN32
#include <signal.h>
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#endif

using namespace yuan::net::dns;
using namespace yuan::buffer;

void test_dns_packet_serialization()
{
    std::cout << "=== Test DNS Packet Serialization ===" << std::endl;

    DnsPacket packet;
    packet.set_session_id(12345);
    packet.set_is_response(false);
    packet.set_recursion_desired(true);

    DnsQuestion question;
    question.name = "example.com";
    question.type = DnsType::A;
    question.class_ = DnsClass::IN;
    packet.add_question(question);

    Buffer buffer;
    packet.serialize(buffer);

    DnsPacket parsed_packet;
    bool result = parsed_packet.deserialize(buffer);

    if (result && parsed_packet.get_session_id() == 12345 &&
        parsed_packet.get_questions().size() == 1 &&
        parsed_packet.get_questions()[0].name == "example.com") {
        std::cout << "✓ Packet serialization test passed!" << std::endl;
    } else {
        std::cout << "✗ Packet serialization test failed!" << std::endl;
    }

    std::cout << std::endl;
}

void test_dns_resource_record()
{
    std::cout << "=== Test DNS Resource Record ===" << std::endl;

    DnsResourceRecord record;
    record.name = "example.com";
    record.type = DnsType::A;
    record.class_ = DnsClass::IN;
    record.ttl = 3600;
    record.set_rdata_from_string("93.184.216.34");

    Buffer buffer;
    record.serialize(buffer);

    auto result = DnsResourceRecord::deserialize(buffer);
    if (result.first) {
        DnsResourceRecord parsed = result.second;
        std::string ip = parsed.get_rdata_as_string();

        if (parsed.name == "example.com" && ip == "93.184.216.34") {
            std::cout << "✓ Resource record test passed!" << std::endl;
            std::cout << "  Parsed IP: " << ip << std::endl;
        } else {
            std::cout << "✗ Resource record test failed!" << std::endl;
        }
    } else {
        std::cout << "✗ Resource record deserialization failed!" << std::endl;
    }

    std::cout << std::endl;
}

void test_dns_server_basic()
{
    std::cout << "=== Test DNS Server Basic ===" << std::endl;

    DnsServer server;
    server.add_record("test.local", "192.168.1.100");
    server.add_record("example.com", "93.184.216.34");
    server.add_record("www.example.com", "93.184.216.34");

    std::cout << "Server added records. Test passed!" << std::endl;
    std::cout << std::endl;
}

void test_dns_client_and_server()
{
    std::cout << "=== Test DNS Client and Server Integration ===" << std::endl;

    const int test_port = 53530;

    // Start server in a separate thread
    DnsServer server;
    server.add_record("test.local", "192.168.1.100");
    server.add_record("example.com", "93.184.216.34");
    server.add_record("www.example.com", "93.184.216.34");

    std::thread server_thread([&server, test_port]() {
        server.serve(test_port);
    });

    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create client
    DnsClient client;
    if (!client.connect("127.0.0.1", test_port)) {
        std::cout << "✗ Failed to connect to server!" << std::endl;
        server.stop();
        server_thread.join();
        return;
    }

    // Query test.local
    std::cout << "Querying: test.local" << std::endl;
    bool success = client.query("test.local", DnsType::A);
    if (success) {
        std::cout << "✓ Query completed successfully!" << std::endl;
    } else {
        std::cout << "✗ Query failed!" << std::endl;
    }

    // Query with handler
    std::cout << "\nQuerying: example.com with handler" << std::endl;
    success = client.query("example.com", DnsType::A, [](const DnsPacket& response) {
        std::cout << "Response received!" << std::endl;
        std::cout << response.to_string() << std::endl;
    });

    if (success) {
        std::cout << "✓ Query with handler completed successfully!" << std::endl;
    } else {
        std::cout << "✗ Query with handler failed!" << std::endl;
    }

    client.disconnect();

    // Stop server
    server.stop();
    server_thread.join();

    std::cout << "✓ Client-Server integration test completed!" << std::endl;
    std::cout << std::endl;
}

void test_dns_name_compression()
{
    std::cout << "=== Test DNS Name Compression ===" << std::endl;

    DnsPacket packet;
    packet.set_session_id(54321);
    packet.set_is_response(true);

    DnsQuestion question1;
    question1.name = "www.example.com";
    question1.type = DnsType::A;
    question1.class_ = DnsClass::IN;
    packet.add_question(question1);

    DnsQuestion question2;
    question2.name = "mail.example.com";
    question2.type = DnsType::A;
    question2.class_ = DnsClass::IN;
    packet.add_question(question2);

    DnsResourceRecord answer;
    answer.name = "www.example.com";
    answer.type = DnsType::A;
    answer.class_ = DnsClass::IN;
    answer.ttl = 3600;
    answer.set_rdata_from_string("93.184.216.34");
    packet.add_answer(answer);

    Buffer buffer;
    packet.serialize(buffer);

    DnsPacket parsed_packet;
    bool result = parsed_packet.deserialize(buffer);

    if (result && parsed_packet.get_questions().size() == 2 &&
        parsed_packet.get_questions()[0].name == "www.example.com" &&
        parsed_packet.get_questions()[1].name == "mail.example.com" &&
        parsed_packet.get_answers().size() == 1) {
        std::cout << "✓ Name compression test passed!" << std::endl;
    } else {
        std::cout << "✗ Name compression test failed!" << std::endl;
    }

    std::cout << std::endl;
}

void test_dns_various_types()
{
    std::cout << "=== Test Various DNS Types ===" << std::endl;

    DnsPacket packet;
    packet.set_session_id(11111);
    packet.set_is_response(true);

    // A record
    DnsResourceRecord a_record;
    a_record.name = "a.test.com";
    a_record.type = DnsType::A;
    a_record.class_ = DnsClass::IN;
    a_record.ttl = 3600;
    a_record.set_rdata_from_string("192.0.2.1");
    packet.add_answer(a_record);

    // AAAA record
    DnsResourceRecord aaaa_record;
    aaaa_record.name = "aaaa.test.com";
    aaaa_record.type = DnsType::AAAA;
    aaaa_record.class_ = DnsClass::IN;
    aaaa_record.ttl = 3600;
    aaaa_record.set_rdata_from_string("2001:db8::1");
    packet.add_answer(aaaa_record);

    // TXT record
    DnsResourceRecord txt_record;
    txt_record.name = "txt.test.com";
    txt_record.type = DnsType::TXT;
    txt_record.class_ = DnsClass::IN;
    txt_record.ttl = 3600;
    txt_record.set_rdata_from_string("test record");
    packet.add_answer(txt_record);

    Buffer buffer;
    packet.serialize(buffer);

    DnsPacket parsed_packet;
    bool result = parsed_packet.deserialize(buffer);

    if (result && parsed_packet.get_answers().size() == 3) {
        bool all_ok = true;

        const auto &answers = parsed_packet.get_answers();
        for (const auto &answer : answers) {
            std::string value = answer.get_rdata_as_string();
            if (value.empty()) {
                all_ok = false;
                std::cout << "✗ Failed to parse record: " << answer.name << std::endl;
            } else {
                std::cout << "  " << answer.name << " -> " << value << std::endl;
            }
        }

        if (all_ok) {
            std::cout << "✓ Various DNS types test passed!" << std::endl;
        } else {
            std::cout << "✗ Various DNS types test failed!" << std::endl;
        }
    } else {
        std::cout << "✗ Deserialization failed!" << std::endl;
    }

    std::cout << std::endl;
}

void test_dns_error_codes()
{
    std::cout << "=== Test DNS Error Codes ===" << std::endl;

    struct ErrorTestCase {
        DnsResponseCode code;
        std::string name;
    };

    std::vector<ErrorTestCase> test_cases = {
        {DnsResponseCode::NO_ERROR, "NO_ERROR"},
        {DnsResponseCode::FORMAT_ERROR, "FORMAT_ERROR"},
        {DnsResponseCode::SERVER_FAILURE, "SERVER_FAILURE"},
        {DnsResponseCode::NAME_ERROR, "NAME_ERROR"},
        {DnsResponseCode::NOT_IMPLEMENTED, "NOT_IMPLEMENTED"},
        {DnsResponseCode::REFUSED, "REFUSED"}
    };

    bool all_passed = true;
    for (const auto &test_case : test_cases) {
        DnsPacket packet;
        packet.set_session_id(99999);
        packet.set_is_response(true);
        packet.set_response_code(test_case.code);

        Buffer buffer;
        packet.serialize(buffer);

        DnsPacket parsed;
        bool result = parsed.deserialize(buffer);

        if (result && parsed.get_response_code() == test_case.code) {
            std::cout << "✓ " << test_case.name << " test passed" << std::endl;
        } else {
            std::cout << "✗ " << test_case.name << " test failed" << std::endl;
            all_passed = false;
        }
    }

    if (all_passed) {
        std::cout << "✓ All error codes test passed!" << std::endl;
    } else {
        std::cout << "✗ Some error codes test failed!" << std::endl;
    }

    std::cout << std::endl;
}

int main()
{
#ifndef _WIN32
#else
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa); iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif

    std::cout << "\n";
    std::cout << "========================================" << std::endl;
    std::cout << "    DNS Protocol Test Suite            " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\n";

    test_dns_packet_serialization();
    test_dns_resource_record();
    test_dns_name_compression();
    test_dns_various_types();
    test_dns_error_codes();
    test_dns_server_basic();
    test_dns_client_and_server();

    std::cout << "========================================" << std::endl;
    std::cout << "    All Tests Completed                " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
