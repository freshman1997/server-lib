#include "dns_client.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>


#ifndef _WIN32
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#endif

using namespace yuan::net::dns;

namespace
{
void print_usage(const char *program_name)
{
    std::cout << "Usage: " << program_name << " <server_ip> <port> <domain1> [domain2] ...\n";
    std::cout << "Example: " << program_name << " 127.0.0.1 53530 example.com google.com\n";
}
} // namespace

int main(int argc, char *argv[])
{
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string server_ip = argv[1];
    const uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::vector<std::string> domains;
    for (int i = 3; i < argc; ++i) {
        domains.emplace_back(argv[i]);
    }

    std::cout << "DNS Client\n";
    std::cout << "Server: " << server_ip << ":" << port << "\n";
    std::cout << "Domains to query: ";
    for (const auto &domain : domains) {
        std::cout << domain << " ";
    }
    std::cout << "\n\n";

    DnsClient client;
    if (!client.connect(server_ip, port)) {
        std::cout << "Failed to connect to DNS server!\n";
        return 1;
    }

    std::cout << "Connected successfully!\n\n";

    for (const auto &domain : domains) {
        std::cout << "----------------------------------------\n";
        std::cout << "Querying: " << domain << "\n";
        std::cout << "----------------------------------------\n";

        const auto response = client.query_async(domain, DnsType::A).execute();
        if (!response.get_answers().empty() ||
            response.get_response_code() != DnsResponseCode::NO_ERROR) {
            std::cout << "Query completed successfully!\n";
        } else {
            std::cout << "Query failed or timed out!\n";
        }

        std::cout << "\n";
    }

    client.disconnect();
    std::cout << "DNS client disconnected.\n";
    return 0;
}
