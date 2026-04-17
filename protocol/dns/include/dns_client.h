#ifndef __NET_DNS_DNS_CLIENT_H__
#define __NET_DNS_DNS_CLIENT_H__

#include "coroutine/task.h"
#include "net/async/async_datagram_client.h"
#include "dns_packet.h"
#include <string>
#include <functional>

namespace yuan::net::dns
{
    using DnsResponseHandler = std::function<void(const DnsPacket & response)>;

    class DnsClient
    {
    public:
        static constexpr uint32_t DEFAULT_TIMEOUT_MS = 5000;

    public:
        DnsClient();
        ~DnsClient();

    public:
        bool connect(const std::string &ip, short port);
        bool connect(const std::string &ip, short port, NetworkRuntime &runtime);
        void disconnect();

        bool query(const std::string &domain, DnsType type = DnsType::A, uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);
        bool query(const std::string &domain, DnsType type, DnsResponseHandler handler, uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);
        yuan::coroutine::Task<DnsPacket> query_async(
            const std::string &domain,
            DnsType type = DnsType::A,
            uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);

        const DnsPacket &get_last_response() const;

    private:
        yuan::buffer::ByteBuffer build_query_packet(const std::string &domain, DnsType type, uint16_t session_id);

    private:
        AsyncDatagramClient client_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;

        uint16_t next_session_id_ = 1;
        DnsPacket last_response_;
    };
}

#endif
