#ifndef __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__
#define __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__

#include "torrent_meta.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace yuan::net::bit_torrent
{

struct TrackerResponse
{
    int32_t interval_ = 0;        // seconds between requests
    int32_t min_interval_ = 0;
    std::string tracker_id_;
    int32_t complete_ = 0;        // seeders
    int32_t incomplete_ = 0;      // leechers
    std::vector<PeerAddress> peers_;

    std::string warning_message_;
};

using TrackerResponseHandler = std::function<void(const TrackerResponse &response)>;

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
                  TrackerResponse *out = nullptr);

    void announce_async(const std::string &tracker_url,
                        const TorrentMeta &meta,
                        int32_t port,
                        TrackerResponseHandler handler,
                        int64_t uploaded = 0,
                        int64_t downloaded = 0,
                        int64_t left = -1);

private:
    std::string build_announce_url(const std::string &tracker_url,
                                   const TorrentMeta &meta,
                                   int32_t port,
                                   int64_t uploaded,
                                   int64_t downloaded,
                                   int64_t left);

    bool parse_response(const std::string &body, TrackerResponse &out);

private:
    std::string peer_id_;
    std::string last_tracker_id_;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_TRACKER_HTTP_TRACKER_H__
