#include "dns_packet.h"
#include <iostream>

#ifndef _WIN32
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
        std::cout << "  Result: " << result << std::endl;
        std::cout << "  Session ID: " << parsed_packet.get_session_id() << std::endl;
        std::cout << "  Questions count: " << parsed_packet.get_questions().size() << std::endl;
    }

    std::cout << std::endl;
}

void test_dns_response()
{
    std::cout << "=== Test DNS Response ===" << std::endl;

    DnsPacket response;
    response.set_session_id(12345);
    response.set_is_response(true);
    response.set_recursion_available(true);
    response.set_authoritative_answer(true);

    DnsQuestion question;
    question.name = "example.com";
    question.type = DnsType::A;
    question.class_ = DnsClass::IN;
    response.add_question(question);

    DnsResourceRecord answer;
    answer.name = "example.com";
    answer.type = DnsType::A;
    answer.class_ = DnsClass::IN;
    answer.ttl = 3600;
    answer.set_rdata_from_string("93.184.216.34");
    response.add_answer(answer);

    Buffer buffer;
    response.serialize(buffer);

    DnsPacket parsed_response;
    bool result = parsed_response.deserialize(buffer);

    if (result &&
        parsed_response.get_session_id() == 12345 &&
        parsed_response.is_response() &&
        parsed_response.get_answers().size() == 1 &&
        parsed_response.get_answers()[0].name == "example.com" &&
        parsed_response.get_answers()[0].get_rdata_as_string() == "93.184.216.34") {
        std::cout << "✓ DNS response test passed!" << std::endl;
    } else {
        std::cout << "✗ DNS response test failed!" << std::endl;
        std::cout << "  Result: " << result << std::endl;
        std::cout << "  Is response: " << parsed_response.is_response() << std::endl;
        std::cout << "  Answers count: " << parsed_response.get_answers().size() << std::endl;
        if (parsed_response.get_answers().size() > 0) {
            std::cout << "  Answer: " << parsed_response.get_answers()[0].get_rdata_as_string() << std::endl;
        }
    }

    std::cout << std::endl;
}

void test_multiple_answers()
{
    std::cout << "=== Test Multiple Answers ===" << std::endl;

    DnsPacket response;
    response.set_session_id(54321);
    response.set_is_response(true);
    response.set_recursion_available(true);

    DnsQuestion question;
    question.name = "www.google.com";
    question.type = DnsType::A;
    question.class_ = DnsClass::IN;
    response.add_question(question);

    DnsResourceRecord answer1;
    answer1.name = "www.google.com";
    answer1.type = DnsType::A;
    answer1.class_ = DnsClass::IN;
    answer1.ttl = 300;
    answer1.set_rdata_from_string("142.250.185.46");
    response.add_answer(answer1);

    DnsResourceRecord answer2;
    answer2.name = "www.google.com";
    answer2.type = DnsType::A;
    answer2.class_ = DnsClass::IN;
    answer2.ttl = 300;
    answer2.set_rdata_from_string("142.250.185.47");
    response.add_answer(answer2);

    Buffer buffer;
    response.serialize(buffer);

    DnsPacket parsed_response;
    bool result = parsed_response.deserialize(buffer);

    if (result &&
        parsed_response.get_answers().size() == 2 &&
        parsed_response.get_answers()[0].get_rdata_as_string() == "142.250.185.46" &&
        parsed_response.get_answers()[1].get_rdata_as_string() == "142.250.185.47") {
        std::cout << "✓ Multiple answers test passed!" << std::endl;
    } else {
        std::cout << "✗ Multiple answers test failed!" << std::endl;
        std::cout << "  Result: " << result << std::endl;
        std::cout << "  Answers count: " << parsed_response.get_answers().size() << std::endl;
    }

    std::cout << std::endl;
}

void test_different_record_types()
{
    std::cout << "=== Test Different Record Types ===" << std::endl;

    DnsPacket response;
    response.set_session_id(67890);
    response.set_is_response(true);

    DnsQuestion question;
    question.name = "test.com";
    question.type = DnsType::A;
    question.class_ = DnsClass::IN;
    response.add_question(question);

    // Test AAAA record
    DnsResourceRecord aaaa_record;
    aaaa_record.name = "test.com";
    aaaa_record.type = DnsType::AAAA;
    aaaa_record.class_ = DnsClass::IN;
    aaaa_record.ttl = 3600;
    aaaa_record.set_rdata_from_string("2001:0db8:85a3:0000:0000:8a2e:0370:7334");
    response.add_answer(aaaa_record);

    // Test TXT record
    DnsResourceRecord txt_record;
    txt_record.name = "test.com";
    txt_record.type = DnsType::TXT;
    txt_record.class_ = DnsClass::IN;
    txt_record.ttl = 3600;
    txt_record.set_rdata_from_string("v=spf1 include:_spf.example.com ~all");
    response.add_answer(txt_record);

    Buffer buffer;
    response.serialize(buffer);

    DnsPacket parsed_response;
    bool result = parsed_response.deserialize(buffer);

    if (result && parsed_response.get_answers().size() == 2) {
        bool aaaa_ok = parsed_response.get_answers()[0].get_rdata_as_string().find("2001") != std::string::npos;
        bool txt_ok = parsed_response.get_answers()[1].get_rdata_as_string().find("v=spf1") != std::string::npos;

        if (aaaa_ok && txt_ok) {
            std::cout << "✓ Different record types test passed!" << std::endl;
        } else {
            std::cout << "✗ Different record types test failed (content mismatch)!" << std::endl;
            std::cout << "  AAAA: " << aaaa_ok << std::endl;
            std::cout << "  TXT: " << txt_ok << std::endl;
        }
    } else {
        std::cout << "✗ Different record types test failed!" << std::endl;
        std::cout << "  Result: " << result << std::endl;
        std::cout << "  Answers count: " << parsed_response.get_answers().size() << std::endl;
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

    std::cout << "DNS Protocol Tests\n" << std::endl;
    std::cout << "==================" << std::endl;

    test_dns_packet_serialization();
    test_dns_response();
    test_multiple_answers();
    test_different_record_types();

    std::cout << "==================" << std::endl;
    std::cout << "All tests completed!" << std::endl;

#ifndef _WIN32
#else
    WSACleanup();
#endif

    return 0;
}
