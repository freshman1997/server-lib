#ifndef __SERVER_HTTP_SERVICE_H__
#define __SERVER_HTTP_SERVICE_H__

#include "http_server.h"
#include "server/proxy/include/server_runtime_host.h"
#include "service.h"
#include "eventbus/event_bus.h"
#include "timer/timer_handle.h"

#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <deque>
#include <unordered_map>
#include <vector>

namespace yuan::server
{

    class HttpService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit HttpService(int port, yuan::net::http::HttpServerConfig config = {});
        ~HttpService() override;

        using ServerConfigurator = std::function<bool(HttpService &)>;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        void set_server_configurator(ServerConfigurator configurator);
        void set_admin_dashboard_enabled(bool enabled);

        yuan::net::http::HttpServer &server();
        const yuan::net::http::HttpServer &server() const;

    private:
        void install_admin_dashboard_routes();
        void subscribe_dashboard_events();
        void unsubscribe_dashboard_events();
        void start_dashboard_push_timer();
        void stop_dashboard_push_timer();
        void publish_dashboard_snapshot();
        bool authorize_admin(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) const;

    private:
        struct DashboardSnapshot
        {
            uint64_t last_event_ms = 0;
            uint64_t bt_peer_connected_total = 0;
            uint64_t bt_piece_completed_total = 0;
            uint64_t bt_piece_completed_bytes = 0;
            uint64_t bt_torrent_completed_total = 0;
            std::string bt_last_info_hash;
            std::string bt_last_torrent_name;
            uint64_t bt_speed_samples = 0;
            double bt_speed_download_sum = 0.0;
        };

        struct DashboardEvent
        {
            uint64_t timestamp_ms = 0;
            std::string name;
            std::string detail;
        };

        int port_;
        yuan::net::http::HttpServerConfig config_;
        std::unique_ptr<yuan::net::http::HttpServer> server_;
        ServerConfigurator server_configurator_;
        ServerRuntimeHost host_;
        yuan::app::RuntimeContext runtime_context_{};
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;
        bool admin_dashboard_enabled_ = true;

        mutable std::mutex dashboard_mutex_;
        DashboardSnapshot dashboard_snapshot_{};
        std::unordered_map<std::string, uint64_t> event_counters_;
        std::unordered_map<std::string, bool> service_states_;
        std::deque<DashboardEvent> recent_events_;
        std::vector<yuan::eventbus::SubscriptionToken> event_tokens_;
        yuan::timer::TimerHandle dashboard_push_timer_;
    };

} // namespace yuan::server

#endif
