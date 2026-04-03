#include "dns_server.h"
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

int main(int argc, char *argv[])
{
#ifndef _WIN32
#else
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa); iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif

    int port = 53530;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    std::cout << "Starting DNS Server on port " << port << "...\n";

    DnsServer server;

    // Add some test records
    server.add_record("localhost", "127.0.0.1");
    server.add_record("test.local", "192.168.1.100");
    server.add_record("www.test.local", "192.168.1.101");
    server.add_record("mail.test.local", "192.168.1.102");

    server.add_record("example.com", "93.184.216.34");
    server.add_record("www.example.com", "93.184.216.34");
    server.add_record("mail.example.com", "93.184.216.35");

    server.add_record("google.com", "142.250.185.46");
    server.add_record("www.google.com", "142.250.185.46");

    // Custom query handler
    server.set_query_handler([](const DnsPacket& query, DnsPacket& response) {
        std::cout << "\n=== Custom Query Handler ===\n";
        std::cout << "Received query for: ";
        for (const auto& q : query.get_questions()) {
            std::cout << q.name << " (type: " << static_cast<int>(q.type) << ") ";
        }
        std::cout << "\n==========================\n";
    });

    std::cout << "\nDNS Records:\n";
    std::cout << "  localhost -> 127.0.0.1\n";
    std::cout << "  test.local -> 192.168.1.100\n";
    std::cout << "  www.test.local -> 192.168.1.101\n";
    std::cout << "  mail.test.local -> 192.168.1.102\n";
    std::cout << "  example.com -> 93.184.216.34\n";
    std::cout << "  www.example.com -> 93.184.216.34\n";
    std::cout << "  mail.example.com -> 93.184.216.35\n";
    std::cout << "  google.com -> 142.250.185.46\n";
    std::cout << "  www.google.com -> 142.250.185.46\n";
    std::cout << "\nPress Ctrl+C to stop the server\n\n";

    // Run server
    server.serve(port);

    return 0;
}
