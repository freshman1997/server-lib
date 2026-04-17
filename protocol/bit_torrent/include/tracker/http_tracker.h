#ifndef __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__
#define __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__

#include "tracker/announce_event.h"
#include "torrent_meta.h"
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <cstdint>

namespace yuan::net::bit_torrent
{

    struct TrackerResponse
    {
        int32_t interval_ = 0; // seconds between requests
        int32_t min_interval_ = 0;
        std::string tracker_id_;
        int32_t complete_ = 0;   // seeders
        int32_t incomplete_ = 0; // leechers
        std::vector<PeerAddress> peers_;

        std::string warning_message_;
    };

    struct ScrapeResponse
    {
        int32_t complete_ = 0;
        int32_t downloaded_ = 0;
        int32_t incomplete_ = 0;
    };

    using TrackerResponseHandler = std::function<void(const TrackerResponse & response)>;
    using ScrapeResponseHandler = std::function<void(const ScrapeResponse & response)>;

    class HttpTracker
    {
    public:
        HttpTracker();
        ~HttpTracker();

    public:
        bool announce(const std::string &tracker_url,
                      const TorrentMeta &meta,
                      int32_t port,
                      int64_t uploaded = 0,
                      int64_t downloaded = 0,
                      int64_t left = -1,
                      TrackerAnnounceEvent event = TrackerAnnounceEvent::started,
                      TrackerResponse *out = nullptr);

        void announce_async(const std::string &tracker_url,
                            const TorrentMeta &meta,
                            int32_t port,
                            TrackerResponseHandler handler,
                            int64_t uploaded = 0,
                            int64_t downloaded = 0,
                            int64_t left = -1,
                            TrackerAnnounceEvent event = TrackerAnnounceEvent::started);

        bool scrape(const std::string &tracker_url,
                    const TorrentMeta &meta,
                    ScrapeResponse *out = nullptr);

        void scrape_async(const std::string &tracker_url,
                          const TorrentMeta &meta,
                          ScrapeResponseHandler handler);

    private:
        std::string build_announce_url(const std::string &tracker_url,
                                       const TorrentMeta &meta,
                                       int32_t port,
                                       int64_t uploaded,
                                       int64_t downloaded,
                                       int64_t left,
                                       TrackerAnnounceEvent event);

        std::string build_scrape_url(const std::string &tracker_url,
                                     const TorrentMeta &meta);

        bool parse_response(const std::string &body, TrackerResponse &out);
        bool parse_scrape_response(const std::string &body, const std::vector<uint8_t> &info_hash, ScrapeResponse &out);

    private:
        std::string peer_id_;
        std::string last_tracker_id_;
        std::vector<std::thread> workers_;
        std::mutex workers_mutex_;
    };

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__
