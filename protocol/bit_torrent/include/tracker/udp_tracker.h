#ifndef __BIT_TORRENT_TRACKER_UDP_TRACKER_H__
#define __BIT_TORRENT_TRACKER_UDP_TRACKER_H__

#include "tracker/announce_event.h"
#include "torrent_meta.h"
#include "coroutine/completion_event.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "net/poller/poller.h"
#include "event/event_loop.h"
#include "buffer/byte_buffer.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>

namespace yuan::net
{
    class Connection;
    class DatagramAcceptor;
}

namespace yuan::net::bit_torrent
{

struct UdpTrackerResponse
{
    int32_t interval_ = 0;
    int32_t complete_ = 0;        // seeders
    int32_t incomplete_ = 0;      // leechers
    std::vector<PeerAddress> peers_;
    std::string error_message_;
    bool is_error = false;
};

using UdpTrackerHandler = std::function<void(const UdpTrackerResponse &)>;

class UdpTracker : public ConnectionHandler
{
public:
    static constexpr int32_t DEFAULT_TIMEOUT_MS = 15000;
    static constexpr int32_t CONNECT_ACTION = 0;
    static constexpr int32_t ANNOUNCE_ACTION = 1;
    static constexpr int32_t ERROR_ACTION = 3;
    static constexpr uint64_t MAGIC_CONNECTION_ID = 0x41727101980ULL;

public:
    UdpTracker();
    ~UdpTracker();

public:
    void on_connected(net::Connection *conn) override;
    void on_error(net::Connection *conn) override;
    void on_read(net::Connection *conn) override;
    void on_write(net::Connection *conn) override;
    void on_close(net::Connection *conn) override;

public:
    void on_timer(timer::Timer *timer);

public:
    bool announce(const std::string &tracker_host,
                  uint16_t tracker_port,
                  const TorrentMeta &meta,
                  int32_t local_port,
                  int64_t uploaded = 0,
                  int64_t downloaded = 0,
                  int64_t left = -1,
                  TrackerAnnounceEvent event = TrackerAnnounceEvent::started,
                  UdpTrackerResponse *out = nullptr);

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
        TrackerAnnounceEvent event = TrackerAnnounceEvent::started);

    void disconnect();

private:
    bool init_runtime(timer::TimerManager *timer_manager, net::EventLoop *ev_loop);
    void send_connect_request();
    void send_announce_request(int64_t uploaded, int64_t downloaded, int64_t left, int32_t local_port, TrackerAnnounceEvent event);
    void handle_connect_response(yuan::buffer::ByteBuffer &buf);
    void handle_announce_response(yuan::buffer::ByteBuffer &buf);
    void handle_error_response(yuan::buffer::ByteBuffer &buf);

    uint32_t next_transaction_id();

private:
    bool setup_connection(const std::string &tracker_host, uint16_t tracker_port);

private:
    std::string peer_id_;
    std::vector<uint8_t> info_hash_;

    uint64_t connection_id_ = 0;
    uint32_t transaction_id_ = 0;
    std::atomic<uint32_t> next_tid_;

    timer::TimerManager *timer_manager_;
    net::Poller *poller_;
    net::EventLoop *ev_loop_;
    net::Connection *connection_;
    net::DatagramAcceptor *acceptor_;
    std::atomic<bool> own_loop_;

    timer::Timer *timeout_timer_;
    yuan::coroutine::CompletionEvent completion_event_;

    UdpTrackerHandler handler_;
    UdpTrackerResponse *sync_result_;

    bool connected_to_tracker_ = false;
    bool waiting_response_ = false;
    int expected_action_ = CONNECT_ACTION;
    bool coroutine_mode_ = false;

    int64_t pending_uploaded_ = 0;
    int64_t pending_downloaded_ = 0;
    int64_t pending_left_ = 0;
    int32_t pending_local_port_ = 0;
    TrackerAnnounceEvent pending_event_ = TrackerAnnounceEvent::started;
    bool is_sync_ = false;
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_TRACKER_UDP_TRACKER_H__
