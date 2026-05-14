#ifndef __NET_DNS_DNS_SERVER_H__
#define __NET_DNS_DNS_SERVER_H__

#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/session/datagram_server_session.h"
#include "dns_packet.h"
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

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
        bool has_record(const std::string &name, DnsType type, const std::string &value) const;

    private:
        void handle_dns_query(ConnectionContext &ctx);
        void create_response(const DnsPacket &query, DnsPacket &response);
        std::vector<DnsResourceRecord> find_records(const std::string &name, DnsType type) const;
        bool has_name(const std::string &name) const;
        static std::string normalize_name(std::string name);

    private:
        int port_;
        std::atomic<bool> running_;
        DnsQueryHandler query_handler_;
        std::map<std::string, std::vector<DnsResourceRecord>> dns_records_;

        DatagramServerSession session_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
    };
}

#endif
