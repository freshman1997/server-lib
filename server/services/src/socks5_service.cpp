#include "socks5_service.h"
#include "logger.h"
#include "net/runtime/network_runtime.h"

namespace yuan::server
{

    Socks5Service::Socks5Service(int port, yuan::net::socks5::Socks5ServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::socks5::Socks5Server>(config_)), host_({ "socks5", "socks5", port })
    {
    }

    Socks5Service::~Socks5Service()
    {
        stop();
    }

    bool Socks5Service::init()
    {
        if (!server_) {
            return false;
        }

        if (handler_) {
            server_->set_handler(handler_);
        }

        server_->set_session_event_callback(
            [this](const std::string &event_name, const yuan::net::socks5::Socks5SessionInfo &info) {
                publish_session_event(event_name, info);
            });

        server_->set_session_state_callback(
            [this](const std::string &event_name, const std::string &client_addr,
                   const std::string &previous_state, const std::string &current_state,
                   const std::string &reason) {
                Socks5SessionStateChangedEvent evt;
                evt.service_name = "socks5";
                evt.client_addr = client_addr;
                evt.previous_state = previous_state;
                evt.current_state = current_state;
                evt.reason = reason;
                host_.publish_custom(events::socks5_session_state_changed, std::move(evt));
            });

        if (shared_runtime_) {
            if (!server_->init(port_, *shared_runtime_)) {
                return false;
            }
        } else {
            if (!server_->init(port_)) {
                return false;
            }
        }

        auto *runtime = server_->runtime();
        if (runtime && config_.idle_timeout_ms > 0) {
            snapshot_timer_ = runtime->schedule_periodic(
                10000, 10000,
                [this]() {
                    const auto &m = server_->metrics();
                    Socks5SessionSnapshotEvent evt;
                    evt.service_name = "socks5";
                    evt.active_sessions = static_cast<uint32_t>(m.active_sessions.load(std::memory_order_relaxed));
                    evt.active_udp_associations = static_cast<uint32_t>(m.active_udp_associations.load(std::memory_order_relaxed));
                    evt.total_accepted = m.accepted_sessions.load(std::memory_order_relaxed);
                    evt.total_rejected = m.rejected_sessions.load(std::memory_order_relaxed);
                    evt.total_completed = m.completed_sessions.load(std::memory_order_relaxed);
                    evt.connect_timeouts = m.connect_timeouts.load(std::memory_order_relaxed);
                    evt.idle_timeouts = m.idle_timeouts.load(std::memory_order_relaxed);
                    evt.closes_by_client = m.closes_by_client.load(std::memory_order_relaxed);
                    evt.closes_by_upstream = m.closes_by_upstream.load(std::memory_order_relaxed);
                    evt.closes_by_ssrf = m.closes_by_ssrf.load(std::memory_order_relaxed);
                    evt.closes_by_acl = m.closes_by_acl.load(std::memory_order_relaxed);
                    host_.publish_custom(events::socks5_session_snapshot, std::move(evt));
                });
        }

        LOG_INFO("[Socks5Service] initialized on port {}, auth={}, connect={}, udp={}",
                 port_,
                 config_.enable_auth ? "on" : "off",
                 config_.enable_connect ? "on" : "off",
                 config_.enable_udp_associate ? "on" : "off");

        return true;
    }

    void Socks5Service::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void Socks5Service::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void Socks5Service::stop()
    {
        if (snapshot_timer_) {
            snapshot_timer_->cancel();
            snapshot_timer_ = nullptr;
        }
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::socks5::Socks5Server &Socks5Service::server()
    {
        return *server_;
    }

    const yuan::net::socks5::Socks5Server &Socks5Service::server() const
    {
        return *server_;
    }

    void Socks5Service::set_handler(yuan::net::socks5::Socks5Handler * handler)
    {
        handler_ = handler;
        if (server_) {
            server_->set_handler(handler);
        }
    }

    const yuan::net::socks5::Socks5ServerMetrics &Socks5Service::metrics() const
    {
        return server_->metrics();
    }

    void Socks5Service::publish_session_event(const std::string &event_name, const yuan::net::socks5::Socks5SessionInfo &info)
    {
        if (event_name == "accepted") {
            Socks5SessionAcceptedEvent evt;
            evt.service_name = "socks5";
            evt.client_addr = info.client_addr;
            evt.command = info.command;
            evt.target_addr = info.target_addr;
            evt.active_sessions = static_cast<uint32_t>(server_->metrics().active_sessions.load(std::memory_order_relaxed));
            host_.publish_custom(events::socks5_session_accepted, std::move(evt));
        } else if (event_name == "rejected") {
            Socks5SessionRejectedEvent evt;
            evt.service_name = "socks5";
            evt.client_addr = info.client_addr;
            evt.command = info.command;
            evt.target_addr = info.target_addr;
            evt.reason = info.close_reason;
            host_.publish_custom(events::socks5_session_rejected, std::move(evt));
        } else if (event_name == "completed") {
            Socks5SessionCompletedEvent evt;
            evt.service_name = "socks5";
            evt.client_addr = info.client_addr;
            evt.command = info.command;
            evt.target_addr = info.target_addr;
            evt.duration_ms = info.duration_ms;
            evt.bytes_up = info.bytes_up;
            evt.bytes_down = info.bytes_down;
            evt.close_reason = info.close_reason;
            host_.publish_custom(events::socks5_session_completed, std::move(evt));
        }
    }

} // namespace yuan::server
