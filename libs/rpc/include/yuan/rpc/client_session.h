#ifndef YUAN_RPC_CLIENT_SESSION_H
#define YUAN_RPC_CLIENT_SESSION_H

#include "core_connection_transport.h"
#include "session.h"
#include "client.h"
#include "transport.h"

#include <memory>
#include <optional>
#include <functional>
#include <utility>

namespace yuan::rpc
{
    class RpcClientSession
    {
    public:
        RpcClientSession() = default;

        explicit RpcClientSession(std::shared_ptr<IFrameTransport> transport)
        {
            bind_transport(std::move(transport));
        }

        explicit RpcClientSession(yuan::net::ConnectionContext context)
        {
            bind_connection(std::move(context));
        }

        void bind_transport(std::shared_ptr<IFrameTransport> transport)
        {
            transport_ = std::move(transport);
            session_ = std::make_shared<RpcSession>(transport_);
            client_ = std::make_unique<Client>(*session_);
            session_->start();
        }

        void on_readable(yuan::net::ConnectionContext &context)
        {
            if (auto transport = core_transport()) {
                transport->get().on_readable(context);
            }
        }

        void on_closed()
        {
            if (auto transport = core_transport()) {
                transport->get().on_closed();
            }
        }

        void bind_connection(yuan::net::ConnectionContext context)
        {
            bind_transport(std::make_shared<CoreConnectionTransport>(std::move(context)));
        }

        [[nodiscard]] bool ready() const
        {
            return static_cast<bool>(session_ && client_);
        }

        bool call(Route route, Bytes payload, ResponseHandler on_response, CallOptions options = {})
        {
            return client_ && client_->call(std::move(route), std::move(payload), std::move(on_response), std::move(options));
        }

        template<typename Request, typename ResponseT>
        bool call_typed(Route route,
                        const Request &request,
                        std::function<void(RpcStatus, ResponseT, std::string)> on_response,
                        CallOptions options = {})
        {
            return client_ && client_->call_typed<Request, ResponseT>(std::move(route), request, std::move(on_response), std::move(options));
        }

        bool push(Route route, Bytes payload, Metadata metadata = {})
        {
            return client_ && client_->push(std::move(route), std::move(payload), std::move(metadata));
        }

        template<typename Event>
        bool push_typed(Route route, const Event &event, Metadata metadata = {})
        {
            return client_ && client_->push_typed<Event>(std::move(route), event, std::move(metadata));
        }

        void poll_timeouts()
        {
            if (session_) {
                session_->poll_timeouts();
            }
        }

        [[nodiscard]] std::shared_ptr<RpcSession> session() const
        {
            return session_;
        }

        [[nodiscard]] Client &client()
        {
            return *client_;
        }

    private:
        std::optional<std::reference_wrapper<CoreConnectionTransport>> core_transport() const
        {
            auto core = std::dynamic_pointer_cast<CoreConnectionTransport>(transport_);
            if (!core) {
                return std::nullopt;
            }
            return *core;
        }

        std::shared_ptr<IFrameTransport> transport_;
        std::shared_ptr<RpcSession> session_;
        std::unique_ptr<Client> client_;
    };
}

#endif
