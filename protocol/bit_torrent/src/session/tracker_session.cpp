#include "session/tracker_session.h"
#include "net/runtime/network_runtime.h"
#include "utils.h"
#include <cstdlib>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    TrackerSession::~TrackerSession()
    {
        running_.store(false);
        join_workers();
    }

    void TrackerSession::configure(TrackerSessionConfig config)
    {
        runtime_ = config.runtime_;
        peer_list_handler_ = std::move(config.peer_list_handler_);
        context_provider_ = std::move(config.context_provider_);
    }

    void TrackerSession::start()
    {
        running_.store(true);
        started_sent_ = false;
        completed_sent_ = false;
    }

    void TrackerSession::stop()
    {
        running_.store(false);
        announce_interval_ = 0;
        consecutive_failures_ = 0;

        if (announce_timer_) {
            announce_timer_->cancel();
            announce_timer_ = nullptr;
        }

        if (announcing_.load()) {
            auto ctx = make_announce_context(TrackerAnnounceEvent::stopped);
            if (ctx.meta_ && runtime_) {
                TorrentMeta meta_copy = *ctx.meta_;
                auto listen_port = ctx.listen_port_;
                auto uploaded = ctx.uploaded_;
                auto downloaded = ctx.downloaded_;
                auto left = ctx.left_;
                auto event = ctx.event_;

                std::thread t([meta_copy = std::move(meta_copy), listen_port, uploaded, downloaded, left, event]() mutable {
                    HttpTracker tracker;
                    TrackerResponse resp;
                    tracker.announce(meta_copy.announce_, meta_copy, listen_port, uploaded, downloaded, left, event, &resp);
                });
                t.detach();
            }
        }

        detach_workers();
    }

    void TrackerSession::announce_now()
    {
        if (!running_.load())
            return;

        announce_interval_ = 0;
        const auto ctx = make_announce_context(TrackerAnnounceEvent::none);
        if (!ctx.meta_)
            return;
        announce_from_context(ctx);

        if (announce_interval_ <= 0) {
            announce_interval_ = DEFAULT_ANNOUNCE_INTERVAL;
        }

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
        if (!ctx.meta_ || !runtime_)
            return;

        if (announcing_.load())
            return;

        announcing_.store(true);

        TorrentMeta meta_copy = *ctx.meta_;
        auto listen_port = ctx.listen_port_;
        auto uploaded = ctx.uploaded_;
        auto downloaded = ctx.downloaded_;
        auto left = ctx.left_;
        auto event = ctx.event_;
        auto runtime = runtime_;

        auto self = shared_from_this();
        std::thread t([
            self,
            meta_copy = std::move(meta_copy),
            listen_port,
            uploaded,
            downloaded,
            left,
            event,
            runtime
        ]() mutable {
            bool any_success = false;
            int32_t new_interval = 0;
            std::vector<PeerAddress> all_peers;

            for (size_t tier = 0; tier < meta_copy.announce_list_.size(); ++tier) {
                bool tier_success = false;
                for (size_t i = 0; i < meta_copy.announce_list_[tier].size(); ++i) {
                    const auto &url = meta_copy.announce_list_[tier][i];

                    if (url.size() >= 4 && url.substr(0, 4) == "http") {
                        HttpTracker local_tracker;
                        TrackerResponse resp;
                        bool ok = local_tracker.announce(
                            url, meta_copy, listen_port, uploaded, downloaded, left, event, &resp);
                        if (ok && !resp.is_error) {
                            tier_success = true;
                            all_peers = std::move(resp.peers_);
                            new_interval = resp.interval_;
                            break;
                        }
                        continue;
                    }

                    if (url.size() >= 3 && url.substr(0, 3) == "udp") {
                        std::string host_port = url.substr(6);
                        auto colon = host_port.find(':');
                        if (colon != std::string::npos) {
                            std::string host = host_port.substr(0, colon);
                            uint16_t port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));

                            UdpTracker udp_tracker;
                            UdpTrackerResponse resp;
                            udp_tracker.announce(host, port, meta_copy, listen_port,
                                                  uploaded, downloaded, left, event, &resp);
                            if (!resp.is_error) {
                                tier_success = true;
                                all_peers = std::move(resp.peers_);
                                new_interval = resp.interval_;
                                break;
                            }
                        }
                        continue;
                    }
                }

                if (tier_success) {
                    any_success = true;
                    break;
                }
            }

            auto shared_peers = std::make_shared<std::vector<PeerAddress>>(std::move(all_peers));
            runtime->dispatch([
                self,
                any_success,
                new_interval,
                shared_peers,
                event
            ]() {
                if (!self->running_.load())
                    return;
                self->on_announce_complete(any_success, new_interval, *shared_peers, event);
            });
        });

        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            workers_.push_back(std::move(t));
        }
    }

    void TrackerSession::on_announce_complete(bool any_success, int32_t interval, const std::vector<PeerAddress> &peers, TrackerAnnounceEvent event)
    {
        announcing_.store(false);

        if (any_success) {
            if (!peers.empty() && peer_list_handler_) {
                peer_list_handler_(peers);
            }
            if (interval > 0) {
                announce_interval_ = interval;
            }
            consecutive_failures_ = 0;
        } else {
            handle_announce_failure();
        }

        if (event == TrackerAnnounceEvent::started) {
            started_sent_ = true;
        } else if (event == TrackerAnnounceEvent::completed) {
            completed_sent_ = true;
        }
    }

    void TrackerSession::schedule_next_announce()
    {
        if (!running_.load() || announce_interval_ <= 0 || !runtime_) {
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

    int32_t TrackerSession::compute_backoff_interval() const
    {
        int32_t interval = RETRY_BASE_INTERVAL;
        for (int32_t i = 0; i < consecutive_failures_; ++i) {
            interval *= 2;
            if (interval >= RETRY_MAX_INTERVAL) {
                return RETRY_MAX_INTERVAL;
            }
        }
        return std::min(interval, RETRY_MAX_INTERVAL);
    }

    void TrackerSession::handle_announce_failure()
    {
        ++consecutive_failures_;
        announce_interval_ = compute_backoff_interval();
    }

    void TrackerSession::join_workers()
    {
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            to_join = std::move(workers_);
        }
        for (auto &t : to_join) {
            if (t.joinable())
                t.join();
        }
    }

    void TrackerSession::detach_workers()
    {
        std::vector<std::thread> to_detach;
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            to_detach = std::move(workers_);
        }
        for (auto &t : to_detach) {
            if (t.joinable())
                t.detach();
        }
    }

} // namespace yuan::net::bit_torrent
