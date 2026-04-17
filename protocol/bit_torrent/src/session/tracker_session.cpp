#include "session/tracker_session.h"
#include "net/runtime/network_runtime.h"
#include "utils.h"
#include <cstdlib>

namespace yuan::net::bit_torrent
{

    void TrackerSession::configure(TrackerSessionConfig config)
    {
        runtime_ = config.runtime_;
        peer_list_handler_ = std::move(config.peer_list_handler_);
        context_provider_ = std::move(config.context_provider_);
    }

    void TrackerSession::start()
    {
        running_ = true;
        started_sent_ = false;
        completed_sent_ = false;
    }

    void TrackerSession::stop()
    {
        if (running_) {
            const auto stop_ctx = make_announce_context(TrackerAnnounceEvent::stopped);
            if (stop_ctx.meta_) {
                announce_from_context(stop_ctx);
            }
        }

        running_ = false;
        announce_interval_ = 0;

        if (announce_timer_) {
            announce_timer_->cancel();
            announce_timer_ = nullptr;
        }
    }

    void TrackerSession::announce_now()
    {
        if (!running_)
            return;

        announce_interval_ = 0;
        const auto ctx = make_announce_context(TrackerAnnounceEvent::none);
        if (!ctx.meta_)
            return;
        announce_from_context(ctx);

        schedule_next_announce();
    }

    TrackerAnnounceContext TrackerSession::make_announce_context(TrackerAnnounceEvent fallback_event) const
    {
        if (!context_provider_) {
            return {};
        }

        auto ctx = context_provider_();
        if (!ctx.meta_) {
            return ctx;
        }

        if (ctx.event_ != TrackerAnnounceEvent::none) {
            return ctx;
        }

        if (fallback_event != TrackerAnnounceEvent::none) {
            ctx.event_ = fallback_event;
            return ctx;
        }

        if (!started_sent_) {
            ctx.event_ = TrackerAnnounceEvent::started;
        } else if (ctx.left_ == 0 && !completed_sent_) {
            ctx.event_ = TrackerAnnounceEvent::completed;
        }

        return ctx;
    }

    void TrackerSession::announce_from_context(const TrackerAnnounceContext & ctx)
    {
        if (!ctx.meta_)
            return;

        for (size_t tier = 0; tier < ctx.meta_->announce_list_.size(); ++tier) {
            for (size_t i = 0; i < ctx.meta_->announce_list_[tier].size(); ++i) {
                const auto &url = ctx.meta_->announce_list_[tier][i];
                if (url.substr(0, 4) == "http") {
                    TrackerResponse resp;
                    http_tracker_.announce(
                        url, *ctx.meta_, ctx.listen_port_, ctx.uploaded_, ctx.downloaded_, ctx.left_, ctx.event_, &resp);
                    on_http_response(resp);
                    break;
                }

                if (url.substr(0, 3) == "udp") {
                    std::string host_port = url.substr(6);
                    auto colon = host_port.find(':');
                    if (colon != std::string::npos) {
                        std::string host = host_port.substr(0, colon);
                        uint16_t port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));

                        UdpTracker udp_tracker;
                        UdpTrackerResponse resp;
                        udp_tracker.announce(host, port, *ctx.meta_, ctx.listen_port_,
                                             ctx.uploaded_, ctx.downloaded_, ctx.left_, ctx.event_, &resp);
                        on_udp_response(resp);
                    }
                    break;
                }
            }
        }

        if (ctx.event_ == TrackerAnnounceEvent::started) {
            started_sent_ = true;
        } else if (ctx.event_ == TrackerAnnounceEvent::completed) {
            completed_sent_ = true;
        }
    }

    void TrackerSession::schedule_next_announce()
    {
        if (!running_ || announce_interval_ <= 0 || !runtime_) {
            return;
        }

        if (announce_timer_) {
            announce_timer_->cancel();
            announce_timer_ = nullptr;
        }

        announce_timer_ = runtime_->schedule(
            static_cast<uint32_t>(announce_interval_ * 1000),
            [this]() {
            announce_now();
            });
    }

    void TrackerSession::on_http_response(const TrackerResponse & resp)
    {
        if (!resp.peers_.empty() && peer_list_handler_) {
            peer_list_handler_(resp.peers_);
        }

        if (resp.interval_ > 0) {
            announce_interval_ = resp.interval_;
        }
    }

    void TrackerSession::on_udp_response(const UdpTrackerResponse & resp)
    {
        if (resp.is_error) {
            return;
        }

        if (!resp.peers_.empty() && peer_list_handler_) {
            peer_list_handler_(resp.peers_);
        }

        if (resp.interval_ > 0) {
            announce_interval_ = resp.interval_;
        }
    }

} // namespace yuan::net::bit_torrent
