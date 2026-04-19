#ifndef __SERVER_PROXY_SERVICE_H__
#define __SERVER_PROXY_SERVICE_H__

#include "server_runtime_host.h"
#include "service.h"
#include "socks5_config.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net
{
    class NetworkRuntime;
}

namespace yuan::server
{
    struct ProxyServiceConfig
    {
        std::string listen_host = "0.0.0.0";
        int port = 3128;
        int max_active_sessions = 4096;
        int max_sessions_per_client = 1024;
        int header_timeout_ms = 15000;
        int idle_timeout_ms = 300000;
        int connect_timeout_ms = 10000;
        int drain_timeout_ms = 5000;
        int session_snapshot_interval_ms = 10000;
        int max_header_bytes = 64 * 1024;
        std::string basic_auth_user;
        std::string basic_auth_password;
        std::vector<std::string> allow_targets;
        std::vector<std::string> deny_targets;
    };

    struct Socks5ServiceConfigFile
    {
        bool enabled = true;
        int port = 1080;
        yuan::net::socks5::Socks5ServerConfig server_config;
    };

    class ProxyService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit ProxyService(ProxyServiceConfig config = {});
        ~ProxyService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        const ProxyServiceConfig &config() const noexcept
        {
            return config_;
        }

    private:
        void serve_loop();
        void reap_finished_sessions();

        ProxyServiceConfig config_;
        std::atomic_bool stop_requested_{ false };
        std::atomic_int active_sessions_{ 0 };
        std::atomic_uint64_t accepted_sessions_{ 0 };
        std::atomic_uint64_t rejected_sessions_{ 0 };
        std::atomic_uint64_t completed_sessions_{ 0 };
        std::atomic_uint64_t next_session_id_{ 1 };
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
        class ProxyServiceData;
        std::unique_ptr<ProxyServiceData> data_;
    };
}

#endif
