#include "session/tracker_session.h"
#include "net/runtime/network_runtime.h"
#include "utils.h"
#include <cstdlib>
#include <algorithm>
#include <chrono>

namespace yuan::net::bit_torrent
{

    namespace
    {
        uint64_t now_ms()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        bool parse_udp_tracker_url(const std::string &url, std::string &host, uint16_t &port)
        {
            static constexpr const char *prefix = "udp://";
            if (url.rfind(prefix, 0) != 0) {
                return false;
            }

            std::string authority = url.substr(6);
            const auto slash = authority.find('/');
            if (slash != std::string::npos) {
                authority = authority.substr(0, slash);
            }
            const auto query = authority.find('?');
            if (query != std::string::npos) {
                authority = authority.substr(0, query);
            }

            if (authority.empty()) {
                return false;
            }

            const auto colon = authority.rfind(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= authority.size()) {
                return false;
            }

            host = authority.substr(0, colon);
            const int parsed_port = std::atoi(authority.substr(colon + 1).c_str());
            if (parsed_port <= 0 || parsed_port > 65535) {
                return false;
            }
            port = static_cast<uint16_t>(parsed_port);
            return true;
        }
    }

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
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            announce_statuses_.clear();
        }
    }

    void TrackerSession::stop()
    {
        TrackerAnnounceContext stopped_ctx;
        if (running_.load()) {
            stopped_ctx = make_announce_context(TrackerAnnounceEvent::stopped);
        }

        running_.store(false);
        announce_interval_ = 0;
        consecutive_failures_ = 0;

        if (announce_timer_) {
            announce_timer_.cancel();
            announce_timer_.reset();
        }

        if (stopped_ctx.meta_ && !stopped_ctx.meta_->announce_.empty()) {
            TorrentMeta meta_copy = *stopped_ctx.meta_;
            auto listen_port = stopped_ctx.listen_port_;
            auto uploaded = stopped_ctx.uploaded_;
            auto downloaded = stopped_ctx.downloaded_;
            auto left = stopped_ctx.left_;
            std::string peer_id = stopped_ctx.peer_id_ ? *stopped_ctx.peer_id_ : std::string();

            std::thread t([meta_copy = std::move(meta_copy), listen_port, uploaded, downloaded, left, peer_id = std::move(peer_id)]() mutable {
                HttpTracker tracker;
                TrackerResponse resp;
                tracker.announce(meta_copy.announce_, meta_copy, listen_port, uploaded, downloaded, left,
                                 TrackerAnnounceEvent::stopped, &resp, peer_id);
            });
            t.detach();
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
        std::string peer_id = ctx.peer_id_ ? *ctx.peer_id_ : std::string();

        auto self = shared_from_this();
        std::thread t([
            self,
            meta_copy = std::move(meta_copy),
            listen_port,
            uploaded,
            downloaded,
            left,
            event,
            runtime,
            peer_id = std::move(peer_id)
        ]() mutable {
            bool any_success = false;
            int32_t new_interval = 0;
            std::vector<PeerAddress> all_peers;
            std::vector<TrackerAnnounceStatus> statuses;

            for (size_t tier = 0; tier < meta_copy.announce_list_.size(); ++tier) {
                bool tier_success = false;
                for (size_t i = 0; i < meta_copy.announce_list_[tier].size(); ++i) {
                    const auto &url = meta_copy.announce_list_[tier][i];

                    if (url.size() >= 4 && url.substr(0, 4) == "http") {
                        HttpTracker local_tracker;
                        TrackerResponse resp;
                        bool ok = local_tracker.announce(
                            url, meta_copy, listen_port, uploaded, downloaded, left, event, &resp, peer_id);
                        TrackerAnnounceStatus status;
                        status.url_ = url;
                        status.success_ = ok && !resp.is_error;
                        status.is_error_ = !status.success_;
                        status.interval_ = resp.interval_;
                        status.peer_count_ = static_cast<int32_t>(resp.peers_.size());
                        status.last_announce_ms_ = now_ms();
                        status.error_message_ = resp.error_message_;
                        statuses.push_back(status);
                        if (ok && !resp.is_error) {
                            tier_success = true;
                            all_peers = std::move(resp.peers_);
                            new_interval = resp.interval_;
                            break;
                        }
                        continue;
                    }

                    if (url.rfind("udp://", 0) == 0) {
                        std::string host;
                        uint16_t port = 0;
                        if (parse_udp_tracker_url(url, host, port)) {
                            UdpTracker udp_tracker;
                            UdpTrackerResponse resp;
                            udp_tracker.announce(host, port, meta_copy, listen_port,
                                                  uploaded, downloaded, left, event, &resp, peer_id);
                            TrackerAnnounceStatus status;
                            status.url_ = url;
                            status.success_ = !resp.is_error;
                            status.is_error_ = resp.is_error;
                            status.interval_ = resp.interval_;
                            status.peer_count_ = static_cast<int32_t>(resp.peers_.size());
                            status.last_announce_ms_ = now_ms();
                            status.error_message_ = resp.error_message_;
                            statuses.push_back(status);
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
                event,
                statuses = std::move(statuses)
            ]() {
                if (!self->running_.load())
                    return;
                {
                    std::lock_guard<std::mutex> lock(self->status_mutex_);
                    self->announce_statuses_ = std::move(statuses);
                }
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
            announce_timer_.cancel();
            announce_timer_.reset();
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

    std::vector<TrackerAnnounceStatus> TrackerSession::announce_statuses() const
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return announce_statuses_;
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
