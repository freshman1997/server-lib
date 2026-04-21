#ifndef __SERVER_SOCKS5_SERVICE_H__
#define __SERVER_SOCKS5_SERVICE_H__

#include "socks5.h"
#include "server_runtime_host.h"
#include "service.h"
#include "server_service_custom_events.h"
#include "timer/timer_util.hpp"

#include <memory>

namespace yuan::server
{

    class Socks5Service : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit Socks5Service(int port, yuan::net::socks5::Socks5ServerConfig config = {});
        ~Socks5Service() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        yuan::net::socks5::Socks5Server &server();
        const yuan::net::socks5::Socks5Server &server() const;

        void set_handler(yuan::net::socks5::Socks5Handler *handler);

        const yuan::net::socks5::Socks5ServerMetrics &metrics() const;

    private:
        void publish_session_event(const std::string &event_name, const yuan::net::socks5::Socks5SessionInfo &info);

        int port_;
        yuan::net::socks5::Socks5ServerConfig config_;
        std::unique_ptr<yuan::net::socks5::Socks5Server> server_;
        yuan::net::socks5::Socks5Handler *handler_ = nullptr;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
        yuan::timer::Timer *snapshot_timer_ = nullptr;
    };

} // namespace yuan::server

#endif
