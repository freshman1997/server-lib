#ifndef __BIT_TORRENT_TRACKER_UDP_TRACKER_H__
#define __BIT_TORRENT_TRACKER_UDP_TRACKER_H__

#include "tracker/announce_event.h"
#include "torrent_meta.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "net/async/async_datagram_client.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "buffer/byte_buffer.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>
#include <random>

namespace yuan::net::bit_torrent
{

    struct UdpTrackerResponse
    {
        int32_t interval_ = 0;
        int32_t complete_ = 0;
        int32_t incomplete_ = 0;
        std::vector<PeerAddress> peers_;
        std::string error_message_;
        bool is_error = false;
    };

    struct UdpScrapeResponse
    {
        int32_t complete_ = 0;
        int32_t downloaded_ = 0;
        int32_t incomplete_ = 0;
        std::string error_message_;
        bool is_error = false;
    };

    using UdpTrackerHandler = std::function<void(const UdpTrackerResponse &)>;
    using UdpScrapeHandler = std::function<void(const UdpScrapeResponse &)>;

    class UdpTracker
    {
    public:
        static constexpr int32_t DEFAULT_TIMEOUT_MS = 15000;
        static constexpr int32_t CONNECT_ACTION = 0;
        static constexpr int32_t ANNOUNCE_ACTION = 1;
        static constexpr int32_t SCRAPE_ACTION = 2;
        static constexpr int32_t ERROR_ACTION = 3;
        static constexpr uint64_t MAGIC_CONNECTION_ID = 0x41727101980ULL;

    public:
        UdpTracker();
        ~UdpTracker();

    public:
        bool announce(const std::string &tracker_host,
                      uint16_t tracker_port,
                      const TorrentMeta &meta,
                      int32_t local_port,
                      int64_t uploaded = 0,
                      int64_t downloaded = 0,
                      int64_t left = -1,
                      TrackerAnnounceEvent event = TrackerAnnounceEvent::started,
                      UdpTrackerResponse *out = nullptr,
                      const std::string &peer_id = "");

        bool announce(const std::string &tracker_host,
                      uint16_t tracker_port,
                      const TorrentMeta &meta,
                      int32_t local_port,
                      UdpTrackerHandler handler,
                      int64_t uploaded = 0,
                      int64_t downloaded = 0,
                      int64_t left = -1,
                      TrackerAnnounceEvent event = TrackerAnnounceEvent::started);

        yuan::coroutine::Task<UdpTrackerResponse> announce_async(
            yuan::coroutine::RuntimeView runtime,
            const std::string &tracker_host,
            uint16_t tracker_port,
            const TorrentMeta &meta,
            int32_t local_port,
            int64_t uploaded = 0,
            int64_t downloaded = 0,
            int64_t left = -1,
            TrackerAnnounceEvent event = TrackerAnnounceEvent::started,
            const std::string &peer_id = "");

        void disconnect();

        bool scrape(const std::string &tracker_host,
                    uint16_t tracker_port,
                    const TorrentMeta &meta,
                    UdpScrapeResponse *out = nullptr);

        bool scrape(const std::string &tracker_host,
                    uint16_t tracker_port,
                    const TorrentMeta &meta,
                    UdpScrapeHandler handler);

        yuan::coroutine::Task<UdpScrapeResponse> scrape_async(
            yuan::coroutine::RuntimeView runtime,
            const std::string &tracker_host,
            uint16_t tracker_port,
            const TorrentMeta &meta);

    private:
        yuan::buffer::ByteBuffer build_connect_request();
        yuan::buffer::ByteBuffer build_announce_request(
            int64_t uploaded, int64_t downloaded, int64_t left,
            int32_t local_port, TrackerAnnounceEvent event,
            const std::string &peer_id);
        yuan::buffer::ByteBuffer build_scrape_request();

        bool parse_connect_response(yuan::buffer::ByteBuffer &buf);
        UdpTrackerResponse parse_announce_response(yuan::buffer::ByteBuffer &buf);
        UdpScrapeResponse parse_scrape_response(yuan::buffer::ByteBuffer &buf);
        UdpTrackerResponse parse_error_response(yuan::buffer::ByteBuffer &buf);

        UdpTrackerResponse parse_response(yuan::buffer::ByteBuffer &buf);

        uint32_t next_transaction_id();

    private:
        std::string peer_id_;
        std::vector<uint8_t> info_hash_;

        uint64_t connection_id_ = 0;
        uint32_t transaction_id_ = 0;
        std::atomic<uint32_t> next_tid_;

        AsyncDatagramClient client_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
    };

} // namespace yuan::net::bit_torrent

#endif
