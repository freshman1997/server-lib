#include "dns_server.h"
#include "dns_client.h"
#include <iostream>
#include <thread>
#include <chrono>

#ifndef _WIN32
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#endif

using namespace yuan::net::dns;

int main()
{
#ifndef _WIN32
#else
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif

    const int PORT = 53540;

    // Start DNS server in a separate thread
    std::thread server_thread([PORT]() {
        DnsServer server;
        server.add_record("example.com", "93.184.216.34");
        server.add_record("www.example.com", "93.184.216.34");
        server.add_record("google.com", "142.250.185.46");
        server.add_record("www.google.com", "142.250.185.46");
        server.add_record("localhost", "127.0.0.1");
        server.add_record("test.local", "192.168.1.100");
        server.serve(PORT);
    });
    server_thread.detach();

    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "=== DNS Client-Server End-to-End Test ===" << std::endl;

    DnsClient client;
    if (!client.connect("127.0.0.1", PORT)) {
        std::cout << "FAIL: Failed to connect to DNS server!" << std::endl;
        WSACleanup();
        return 1;
    }
    std::cout << "OK: Connected to DNS server on port " << PORT << std::endl;

    // Query 1: example.com
    {
        std::cout << "\n--- Query 1: example.com ---" << std::endl;
        bool ok = client.query("example.com", DnsType::A);
        if (ok && client.get_last_response().get_answers().size() > 0) {
            std::string ip = client.get_last_response().get_answers()[0].get_rdata_as_string();
            std::cout << "OK: example.com -> " << ip << std::endl;
        } else {
            std::cout << "FAIL: Query example.com returned no answer" << std::endl;
        }
    }

    // Query 2: google.com
    {
        std::cout << "\n--- Query 2: google.com ---" << std::endl;
        bool ok = client.query("google.com", DnsType::A);
        if (ok && client.get_last_response().get_answers().size() > 0) {
            std::string ip = client.get_last_response().get_answers()[0].get_rdata_as_string();
            std::cout << "OK: google.com -> " << ip << std::endl;
        } else {
            std::cout << "FAIL: Query google.com returned no answer" << std::endl;
        }
    }

    // Query 3: localhost
    {
        std::cout << "\n--- Query 3: localhost ---" << std::endl;
        bool ok = client.query("localhost", DnsType::A);
        if (ok && client.get_last_response().get_answers().size() > 0) {
            std::string ip = client.get_last_response().get_answers()[0].get_rdata_as_string();
            std::cout << "OK: localhost -> " << ip << std::endl;
        } else {
            std::cout << "FAIL: Query localhost returned no answer" << std::endl;
        }
    }

    // Query 4: unknown domain (should fail gracefully)
    {
        std::cout << "\n--- Query 4: unknown.test.local (expect NXDOMAIN) ---" << std::endl;
        bool ok = client.query("unknown.test.local", DnsType::A);
        if (ok) {
            auto rcode = static_cast<int>(client.get_last_response().get_response_code());
            if (rcode == 3) { // NXDOMAIN
                std::cout << "OK: Got expected NXDOMAIN response" << std::endl;
            } else {
                std::cout << "FAIL: Expected NXDOMAIN (3), got " << rcode << std::endl;
            }
        } else {
            std::cout << "FAIL: Query returned false (no response received)" << std::endl;
        }
    }

    client.disconnect();
    std::cout << "\n=== All queries completed ===" << std::endl;

#ifndef _WIN32
#else
    WSACleanup();
#endif

    return 0;
}
