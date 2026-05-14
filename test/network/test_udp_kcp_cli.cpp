#include "dns_client.h"
#include "dns_server.h"

#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    bool expect(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : 19091;

    std::thread([port]() {
        yuan::net::dns::DnsServer server;
        server.add_record("kcp.test.local", "10.20.30.40");
        server.serve(port);
    }).detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    yuan::net::dns::DnsClient cli;
    if (!expect(cli.connect(host, static_cast<short>(port)), "dns client failed to connect")) {
        return 1;
    }

    if (!expect(cli.query("kcp.test.local", yuan::net::dns::DnsType::A, 1000), "dns query failed")) {
        cli.disconnect();
        return 1;
    }

    const auto &response = cli.get_last_response();
    if (!expect(!response.get_answers().empty(), "dns response should include answers")) {
        cli.disconnect();
        return 1;
    }

    if (!expect(response.get_answers().front().get_rdata_as_string() == "10.20.30.40", "dns answer ip mismatch")) {
        cli.disconnect();
        return 1;
    }

    if (!expect(cli.query("missing.kcp.test.local", yuan::net::dns::DnsType::A, 1000), "dns NXDOMAIN query should still receive response")) {
        cli.disconnect();
        return 1;
    }

    if (!expect(cli.get_last_response().get_response_code() == yuan::net::dns::DnsResponseCode::NAME_ERROR,
                "missing host should return NAME_ERROR")) {
        cli.disconnect();
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        if (!expect(cli.query("kcp.test.local", yuan::net::dns::DnsType::A, 1000), "repeated dns query failed")) {
            cli.disconnect();
            return 1;
        }
    }

    cli.disconnect();
    std::cout << "udp kcp client smoke test passed\n";
    return 0;
}
