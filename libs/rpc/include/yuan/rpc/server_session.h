#ifndef YUAN_RPC_SERVER_SESSION_H
#define YUAN_RPC_SERVER_SESSION_H

#include "core_connection_transport.h"
#include "server.h"
#include "session.h"
#include "transport.h"

#include <memory>
#include <optional>
#include <functional>
#include <utility>

namespace yuan::rpc
{
    class RpcServerSession
    {
    public:
        RpcServerSession() = default;

        explicit RpcServerSession(Server &server)
            : server_(&server)
        {
        }

        RpcServerSession(Server &server, std::shared_ptr<IFrameTransport> transport)
            : server_(&server)
        {
            bind_transport(std::move(transport));
        }

        RpcServerSession(Server &server, yuan::net::ConnectionContext context)
            : server_(&server)
        {
            bind_connection(std::move(context));
        }

        void set_server(Server &server)
        {
            server_ = &server;
            if (session_) {
                session_->set_server(server_);
            }
        }

        void bind_transport(std::shared_ptr<IFrameTransport> transport)
        {
            transport_ = std::move(transport);
            session_ = std::make_shared<RpcSession>(transport_, server_);
            session_->start();
        }

        void bind_connection(yuan::net::ConnectionContext context)
        {
            bind_transport(std::make_shared<CoreConnectionTransport>(std::move(context)));
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

        [[nodiscard]] bool ready() const
        {
            return static_cast<bool>(session_ && server_);
        }

        [[nodiscard]] std::shared_ptr<RpcSession> session() const
        {
            return session_;
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

        Server *server_ = nullptr;
        std::shared_ptr<IFrameTransport> transport_;
        std::shared_ptr<RpcSession> session_;
    };
}

#endif
