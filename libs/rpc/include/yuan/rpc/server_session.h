#ifndef YUAN_RPC_SERVER_SESSION_H
#define YUAN_RPC_SERVER_SESSION_H

#include "server.h"
#include "session.h"
#include "transport.h"

#include <memory>
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

        [[nodiscard]] bool ready() const
        {
            return static_cast<bool>(session_ && server_);
        }

        [[nodiscard]] std::shared_ptr<RpcSession> session() const
        {
            return session_;
        }

    private:
        Server *server_ = nullptr;
        std::shared_ptr<IFrameTransport> transport_;
        std::shared_ptr<RpcSession> session_;
    };
}

#endif
