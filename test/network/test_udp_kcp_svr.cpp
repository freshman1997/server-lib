#include "dns_server.h"
#include "dns_client.h"

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

int main()
{
    constexpr int kPort = 19090;

    std::thread([kPort]() {
        yuan::net::dns::DnsServer server;
        server.add_record("kcp.test.local", "10.20.30.40");
        server.serve(kPort);
    }).detach();

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    yuan::net::dns::DnsClient client;
    if (!expect(client.connect("127.0.0.1", static_cast<short>(kPort)), "dns client failed to connect to local server")) {
        return 1;
    }

    if (!expect(client.query("kcp.test.local", yuan::net::dns::DnsType::A, 1200), "dns query to test server failed")) {
        client.disconnect();
        return 1;
    }

    const auto &response = client.get_last_response();
    if (!expect(!response.get_answers().empty(), "dns query should return at least one answer")) {
        client.disconnect();
        return 1;
    }

    if (!expect(response.get_answers().front().get_rdata_as_string() == "10.20.30.40", "dns response ip mismatch")) {
        client.disconnect();
        return 1;
    }

    if (!expect(client.query("missing.kcp.test.local", yuan::net::dns::DnsType::A, 1200), "dns NXDOMAIN query should still get response")) {
        client.disconnect();
        return 1;
    }

    if (!expect(client.get_last_response().get_response_code() == yuan::net::dns::DnsResponseCode::NAME_ERROR,
                "missing host should return NAME_ERROR")) {
        client.disconnect();
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        if (!expect(client.query("kcp.test.local", yuan::net::dns::DnsType::A, 1200), "repeated dns query failed")) {
            client.disconnect();
            return 1;
        }
    }

    client.disconnect();
    std::cout << "udp kcp server smoke test passed\n";
    return 0;
}
