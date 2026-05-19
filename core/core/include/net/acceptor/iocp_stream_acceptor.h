#ifndef __YUAN_NET_ACCEPTOR_IOCP_STREAM_ACCEPTOR_H__
#define __YUAN_NET_ACCEPTOR_IOCP_STREAM_ACCEPTOR_H__

#include "net/acceptor/stream_acceptor.h"
#include "net/iocp/iocp_tcp_engine.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"

#include <memory>
#include <mutex>
#include <string>

namespace yuan::net
{

    class IocpStreamAcceptor : public StreamAcceptor
    {
    public:
        IocpStreamAcceptor(std::string host,
                           uint16_t port,
                           NetworkRuntime &runtime,
                           ListenOptions options);
        ~IocpStreamAcceptor() override;

        IocpStreamAcceptor(const IocpStreamAcceptor &) = delete;
        IocpStreamAcceptor &operator=(const IocpStreamAcceptor &) = delete;

        bool listen() override;
        bool listen(int backlog) override;
        void close() override;
        void update_channel() override;

        Channel *listener_channel() const override;

        void on_read_event() override;
        void on_write_event() override;
        void set_event_handler(EventHandler *eventHandler) override;
        void set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler) override;
        ConnectionHandler *connection_handler() const override;
        std::shared_ptr<ConnectionHandler> connection_handler_owner() const override;
        void set_ssl_module(std::shared_ptr<SSLModule> module) override;

        uint16_t local_port() const noexcept;

    private:
        struct State;
        class RuntimeDispatchHandler;

        std::string host_;
        uint16_t port_ = 0;
        NetworkRuntime *runtime_ = nullptr;
        ListenOptions options_;
        EventHandler *event_handler_ = nullptr;
        IocpTcpEngine engine_;
        std::shared_ptr<State> state_;
        std::shared_ptr<RuntimeDispatchHandler> dispatch_handler_;
        std::shared_ptr<SSLModule> ssl_module_;
    };

} // namespace yuan::net

#endif
