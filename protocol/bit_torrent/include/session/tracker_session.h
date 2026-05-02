#ifndef __BIT_TORRENT_SESSION_TRACKER_SESSION_H__
#define __BIT_TORRENT_SESSION_TRACKER_SESSION_H__

#include "tracker/announce_event.h"
#include "torrent_meta.h"
#include "tracker/http_tracker.h"
#include "tracker/udp_tracker.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yuan::net::bit_torrent
{

    struct TrackerAnnounceContext
    {
        const TorrentMeta *meta_ = nullptr;
        int32_t listen_port_ = 0;
        int64_t uploaded_ = 0;
        int64_t downloaded_ = 0;
        int64_t left_ = -1;
        TrackerAnnounceEvent event_ = TrackerAnnounceEvent::none;
    };

    using TrackerPeerListHandler = std::function<void(const std::vector<PeerAddress> &)>;
    using TrackerContextProvider = std::function<TrackerAnnounceContext()>;

    struct TrackerSessionConfig
    {
        net::NetworkRuntime *runtime_ = nullptr;
        TrackerPeerListHandler peer_list_handler_;
        TrackerContextProvider context_provider_;
    };

    class TrackerSession : public std::enable_shared_from_this<TrackerSession>
    {
    public:
        TrackerSession() = default;
        ~TrackerSession();

        void configure(TrackerSessionConfig config);
        void set_runtime(net::NetworkRuntime *runtime)
        {
            runtime_ = runtime;
        }
        void set_peer_list_handler(TrackerPeerListHandler handler)
        {
            peer_list_handler_ = std::move(handler);
        }
        void set_context_provider(TrackerContextProvider provider)
        {
            context_provider_ = std::move(provider);
        }

        void start();
        void stop();
        void announce_now();

        bool is_running() const
        {
            return running_;
        }

    private:
        void announce_from_context(const TrackerAnnounceContext &ctx);
        void schedule_next_announce();
        void on_announce_complete(bool any_success, int32_t interval, const std::vector<PeerAddress> &peers, TrackerAnnounceEvent event);
        TrackerAnnounceContext make_announce_context(TrackerAnnounceEvent fallback_event) const;
        int32_t compute_backoff_interval() const;
        void handle_announce_failure();
        void join_workers();
        void detach_workers();

    private:
        net::NetworkRuntime *runtime_ = nullptr;
        timer::Timer *announce_timer_ = nullptr;
        TrackerPeerListHandler peer_list_handler_;
        TrackerContextProvider context_provider_;
        int32_t announce_interval_ = 0;
        int32_t consecutive_failures_ = 0;
        std::atomic<bool> running_{false};
        std::atomic<bool> announcing_{false};
        bool started_sent_ = false;
        bool completed_sent_ = false;
        std::vector<std::thread> workers_;
        std::mutex worker_mutex_;

        static constexpr int32_t RETRY_BASE_INTERVAL = 5;
        static constexpr int32_t RETRY_MAX_INTERVAL = 600;
        static constexpr int32_t DEFAULT_ANNOUNCE_INTERVAL = 1800;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_SESSION_TRACKER_SESSION_H__
