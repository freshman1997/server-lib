#ifndef YUAN_GAME_SERVER_TUNNEL_TUNNEL_SERVICE_H
#define YUAN_GAME_SERVER_TUNNEL_TUNNEL_SERVICE_H

#include "messaging/tunnel_messages.h"

#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    class TunnelService : public ServiceNode
    {
    public:
        using ReplyHandler = std::function<void(yuan::rpc::Response)>;

        explicit TunnelService(ServiceAddress address);

        bool register_endpoint(ServiceAddress address, yuan::rpc::Server &server);

        bool register_endpoint_handler(ServiceAddress address, std::function<yuan::rpc::Response(yuan::rpc::Message)> handler);

        bool register_endpoint_handler(PackedGameServiceId service_id, std::function<yuan::rpc::Response(yuan::rpc::Message)> handler);

        bool unregister_endpoint(const ServiceAddress &address);

        yuan::rpc::Response forward(TunnelEnvelope envelope);

        yuan::rpc::Response forward_all_of_type(TunnelEnvelope envelope);

        yuan::rpc::Message make_forward_message(const TunnelEnvelope &envelope) const;

        bool forward_async(TunnelEnvelope envelope, ReplyHandler on_reply);

        yuan::rpc::Response handle_reply(TunnelReply reply);

        [[nodiscard]] std::size_t endpoint_count() const;

        [[nodiscard]] std::size_t pending_reply_count() const;

    private:
        struct PendingReply
        {
            std::string source;
            ReplyHandler handler;
        };

        struct Endpoint
        {
            ServiceAddress address;
            std::function<yuan::rpc::Response(yuan::rpc::Message)> handler;
        };

        std::optional<Endpoint> select_endpoint(const TunnelEnvelope &envelope);

        static std::string pending_key(yuan::rpc::RequestId request_id, yuan::rpc::ContinuationId continuation_id);

        static std::string tunnel_route_key(PackedGameServiceId service_id, const std::string &fallback);

        mutable std::mutex mutex_;
        std::unordered_map<std::string, Endpoint> endpoints_;
        std::unordered_map<ServiceTypeId, std::vector<std::string>> type_index_;
        std::unordered_map<std::string, PendingReply> pending_replies_;
        std::mt19937 random_{std::random_device{}()};
    };

    class TunnelCluster
    {
    public:
        bool add(std::shared_ptr<TunnelService> tunnel);

        bool register_endpoint(ServiceAddress address, yuan::rpc::Server &server);

        yuan::rpc::Response forward(TunnelEnvelope envelope);

        [[nodiscard]] std::size_t size() const;

    private:
        std::vector<std::shared_ptr<TunnelService>> tunnels_;
        std::size_t next_ = 0;
    };
}

#endif
