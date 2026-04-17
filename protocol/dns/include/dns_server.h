#ifndef __NET_DNS_DNS_SERVER_H__
#define __NET_DNS_DNS_SERVER_H__

#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/session/datagram_server_session.h"
#include "dns_packet.h"
#include <functional>
#include <map>
#include <string>

namespace yuan::net::dns
{
    using DnsQueryHandler = std::function<void(const DnsPacket & query, DnsPacket & response)>;

    class DnsServer
    {
    public:
        DnsServer();
        ~DnsServer();

    public:
        bool serve(int port);
        bool serve(int port, NetworkRuntime &runtime);
        void stop();

        void set_query_handler(DnsQueryHandler handler);
        void add_record(const std::string &name, const std::string &ip, DnsType type = DnsType::A);

    private:
        void handle_dns_query(ConnectionContext &ctx);
        void create_response(const DnsPacket &query, DnsPacket &response);
        DnsResourceRecord find_record(const std::string &name, DnsType type);

    private:
        int port_;
        std::atomic<bool> running_;
        DnsQueryHandler query_handler_;
        std::map<std::string, DnsResourceRecord> dns_records_;

        DatagramServerSession session_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
    };
}

#endif
