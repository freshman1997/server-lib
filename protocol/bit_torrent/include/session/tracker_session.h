#ifndef __BIT_TORRENT_SESSION_TRACKER_SESSION_H__
#define __BIT_TORRENT_SESSION_TRACKER_SESSION_H__

#include "tracker/announce_event.h"
#include "torrent_meta.h"
#include "tracker/http_tracker.h"
#include "tracker/udp_tracker.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include <cstdint>
#include <functional>
#include <string>
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
    timer::TimerManager *timer_manager_ = nullptr;
    TrackerPeerListHandler peer_list_handler_;
    TrackerContextProvider context_provider_;
};

class TrackerSession
{
public:
    TrackerSession() = default;

    void configure(TrackerSessionConfig config);
    void set_timer_manager(timer::TimerManager *timer_manager) { timer_manager_ = timer_manager; }
    void set_peer_list_handler(TrackerPeerListHandler handler) { peer_list_handler_ = std::move(handler); }
    void set_context_provider(TrackerContextProvider provider) { context_provider_ = std::move(provider); }

    void start();
    void stop();
    void announce_now();

    bool is_running() const { return running_; }

private:
    void announce_from_context(const TrackerAnnounceContext &ctx);
    void schedule_next_announce();
    void on_http_response(const TrackerResponse &resp);
    void on_udp_response(const UdpTrackerResponse &resp);
    TrackerAnnounceContext make_announce_context(TrackerAnnounceEvent fallback_event) const;

private:
    timer::TimerManager *timer_manager_ = nullptr;
    timer::Timer *announce_timer_ = nullptr;
    TrackerPeerListHandler peer_list_handler_;
    TrackerContextProvider context_provider_;
    HttpTracker http_tracker_;
    int32_t announce_interval_ = 0;
    bool running_ = false;
    bool started_sent_ = false;
    bool completed_sent_ = false;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_SESSION_TRACKER_SESSION_H__
