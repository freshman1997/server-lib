#include "tracker/udp_tracker.h"
#include "utils.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/udp_connection.h"
#include "net/socket/socket.h"
#include "net/connection/connection.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"
#include "buffer/pool.h"
#include <cstring>
#include <random>

namespace yuan::net::bit_torrent
{

UdpTracker::UdpTracker()
    : peer_id_(generate_peer_id()),
      timer_manager_(nullptr),
      poller_(nullptr),
      ev_loop_(nullptr),
      connection_(nullptr),
      acceptor_(nullptr),
      own_loop_(false),
      timeout_timer_(nullptr),
      sync_result_(nullptr)
{
    std::random_device rd;
    next_tid_ = rd();
}

UdpTracker::~UdpTracker() { disconnect(); }

void UdpTracker::disconnect()
{
    waiting_response_ = false;
    connected_to_tracker_ = false;

    if (timeout_timer_)
    {
        timeout_timer_->cancel();
        timeout_timer_ = nullptr;
    }

    connection_ = nullptr;

    if (acceptor_)
    {
        acceptor_->close();
        delete acceptor_;
        acceptor_ = nullptr;
    }

    if (own_loop_ && ev_loop_)
    {
        ev_loop_->quit();
        ev_loop_ = nullptr;
        poller_ = nullptr;
        if (timer_manager_)
        {
            delete timer_manager_;
            timer_manager_ = nullptr;
        }
    }
}

uint32_t UdpTracker::next_transaction_id()
{
    return ++next_tid_;
}

void UdpTracker::on_connected(net::Connection *conn) {}

void UdpTracker::on_error(net::Connection *conn) {}

void UdpTracker::on_write(net::Connection *conn) {}

void UdpTracker::on_close(net::Connection *conn)
{
    if (waiting_response_)
    {
        waiting_response_ = false;
        UdpTrackerResponse err_resp;
        err_resp.is_error = true;
        err_resp.error_message_ = "connection closed";
        disconnect();
        if (handler_)
            handler_(err_resp);
        else if (sync_result_)
            *sync_result_ = err_resp;
    }
}

void UdpTracker::on_timer(timer::Timer *timer)
{
    if (waiting_response_)
    {
        waiting_response_ = false;
        UdpTrackerResponse err_resp;
        err_resp.is_error = true;
        err_resp.error_message_ = "timeout";
        disconnect();
        if (handler_)
            handler_(err_resp);
        else if (sync_result_)
            *sync_result_ = err_resp;
    }
}

void UdpTracker::on_read(net::Connection *conn)
{
    auto *buf = conn->get_input_buff();
    if (!buf || buf->readable_bytes() < 4) return;

    const char *data = buf->peek();
    int32_t action = (static_cast<uint8_t>(data[0]) << 24) |
                     (static_cast<uint8_t>(data[1]) << 16) |
                     (static_cast<uint8_t>(data[2]) << 8) |
                     static_cast<uint8_t>(data[3]);

    switch (action)
    {
    case CONNECT_ACTION:
        if (buf->readable_bytes() >= 16)
            handle_connect_response(buf);
        break;
    case ANNOUNCE_ACTION:
        if (buf->readable_bytes() >= 20)
            handle_announce_response(buf);
        break;
    case ERROR_ACTION:
        handle_error_response(buf);
        break;
    default:
        break;
    }
}

void UdpTracker::send_connect_request()
{
    connection_id_ = MAGIC_CONNECTION_ID;
    transaction_id_ = next_transaction_id();
    expected_action_ = CONNECT_ACTION;
    waiting_response_ = true;

    auto *buf = buffer::BufferedPool::get_instance()->allocate(16);
    // connection_id (8 bytes, big-endian)
    for (int i = 7; i >= 0; i--)
    {
        buf->write_uint8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
    }
    buf->write_int32(0); // action = connect
    buf->write_int32(static_cast<int32_t>(transaction_id_));

    if (connection_)
        connection_->write_and_flush(buf);
}

void UdpTracker::send_announce_request(int64_t uploaded, int64_t downloaded, int64_t left, int32_t local_port)
{
    transaction_id_ = next_transaction_id();
    expected_action_ = ANNOUNCE_ACTION;
    waiting_response_ = true;

    auto *buf = buffer::BufferedPool::get_instance()->allocate(98);
    // connection_id (8 bytes, big-endian)
    for (int i = 7; i >= 0; i--)
    {
        buf->write_uint8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
    }
    buf->write_int32(1); // action = announce
    buf->write_int32(static_cast<int32_t>(transaction_id_));
    buf->write_string(reinterpret_cast<const char *>(info_hash_.data()), 20);
    buf->write_string(peer_id_.data(), 20);
    buf->write_int64(downloaded);
    buf->write_int64(left);
    buf->write_int64(uploaded);
    buf->write_int32(0); // event = started
    buf->write_int32(0); // IP = 0
    buf->write_int32(static_cast<int32_t>(next_tid_)); // key
    buf->write_int32(-1); // num_want = as many as possible
    buf->write_uint16(static_cast<uint16_t>(local_port));

    if (connection_)
        connection_->write_and_flush(buf);
}

void UdpTracker::handle_connect_response(buffer::Buffer *buf)
{
    if (buf->readable_bytes() < 16) return;

    int32_t action = buf->read_int32();
    int32_t tid = buf->read_int32();

    if (action != CONNECT_ACTION || tid != static_cast<int32_t>(transaction_id_))
        return;

    // Read connection_id (8 bytes, big-endian)
    connection_id_ = 0;
    for (int i = 0; i < 8; i++)
    {
        connection_id_ = (connection_id_ << 8) | static_cast<uint8_t>(buf->read_int8());
    }

    connected_to_tracker_ = true;
    waiting_response_ = false;
    buf->reset();

    send_announce_request(pending_uploaded_, pending_downloaded_, pending_left_, pending_local_port_);
}

void UdpTracker::handle_announce_response(buffer::Buffer *buf)
{
    if (buf->readable_bytes() < 20) return;

    int32_t action = buf->read_int32();
    int32_t tid = buf->read_int32();

    if (action != ANNOUNCE_ACTION || tid != static_cast<int32_t>(transaction_id_))
        return;

    UdpTrackerResponse resp;
    resp.interval_ = buf->read_int32();
    resp.incomplete_ = buf->read_int32();
    resp.complete_ = buf->read_int32();

    size_t remaining = buf->readable_bytes();
    size_t peer_count = remaining / 6;

    for (size_t i = 0; i < peer_count; i++)
    {
        PeerAddress addr;
        uint8_t b0 = static_cast<uint8_t>(buf->read_int8());
        uint8_t b1 = static_cast<uint8_t>(buf->read_int8());
        uint8_t b2 = static_cast<uint8_t>(buf->read_int8());
        uint8_t b3 = static_cast<uint8_t>(buf->read_int8());
        addr.ip_ = std::to_string(b0) + "." + std::to_string(b1) + "." +
                   std::to_string(b2) + "." + std::to_string(b3);
        addr.port_ = buf->read_uint16();
        resp.peers_.push_back(addr);
    }

    waiting_response_ = false;
    buf->reset();

    if (handler_)
        handler_(resp);
    else if (sync_result_)
        *sync_result_ = resp;
}

void UdpTracker::handle_error_response(buffer::Buffer *buf)
{
    if (buf->readable_bytes() < 8) return;

    int32_t action = buf->read_int32();
    int32_t tid = buf->read_int32();

    std::string error_msg(buf->peek(), buf->readable_bytes());
    buf->reset();

    waiting_response_ = false;

    UdpTrackerResponse resp;
    resp.is_error = true;
    resp.error_message_ = error_msg;

    if (handler_)
        handler_(resp);
    else if (sync_result_)
        *sync_result_ = resp;
}

// Internal helper: setup socket, acceptor, event loop, and UDP connection
bool UdpTracker::setup_connection(const std::string &tracker_host, uint16_t tracker_port)
{
    // Create event loop
    own_loop_ = true;
    poller_ = new net::SelectPoller();
    timer_manager_ = new timer::WheelTimerManager();
    ev_loop_ = new net::EventLoop(poller_, timer_manager_);

    // Create UDP socket (bind to 0.0.0.0:0 for auto-assign)
    auto *sock = new net::Socket("0.0.0.0", 0, true);
    if (!sock->valid())
    {
        delete sock;
        disconnect();
        return false;
    }
    sock->set_reuse(true);
    sock->set_none_block(true);

    acceptor_ = new net::UdpAcceptor(sock, timer_manager_);
    if (!acceptor_->listen())
    {
        delete acceptor_;
        acceptor_ = nullptr;
        delete sock;
        disconnect();
        return false;
    }

    acceptor_->set_connection_handler(this);
    acceptor_->set_event_handler(ev_loop_);

    // Create UDP connection to tracker (same pattern as DnsClient)
    auto *instance = acceptor_->get_udp_instance();
    net::InetAddress tracker_addr(tracker_host, tracker_port);
    const auto &res = instance->on_recv(tracker_addr);
    if (!res.first || !res.second)
    {
        disconnect();
        return false;
    }

    auto *conn = static_cast<net::UdpConnection *>(res.second);
    conn->set_connection_handler(this);
    conn->set_event_handler(ev_loop_);
    conn->set_instance_handler(instance);
    conn->set_connection_state(net::ConnectionState::connected);
    instance->enable_rw_events();
    connection_ = conn;

    return true;
}

bool UdpTracker::announce(const std::string &tracker_host,
                          uint16_t tracker_port,
                          const TorrentMeta &meta,
                          int32_t local_port,
                          int64_t uploaded,
                          int64_t downloaded,
                          int64_t left,
                          UdpTrackerResponse *out)
{
    info_hash_ = meta.info_hash_;
    sync_result_ = out ? out : new UdpTrackerResponse();
    handler_ = nullptr;
    is_sync_ = true;

    pending_uploaded_ = uploaded;
    pending_downloaded_ = downloaded;
    pending_left_ = left;
    pending_local_port_ = local_port;

    if (!setup_connection(tracker_host, tracker_port))
    {
        delete sync_result_;
        if (!out) sync_result_ = nullptr;
        return false;
    }

    // Set timeout
    timeout_timer_ = timer_manager_->timeout(DEFAULT_TIMEOUT_MS, this);

    // Send connect request via queue_in_loop
    ev_loop_->queue_in_loop([this]()
    {
        send_connect_request();
    });

    ev_loop_->loop();

    bool ok = !sync_result_->is_error;
    if (!out)
    {
        delete sync_result_;
        sync_result_ = nullptr;
    }
    return ok;
}

bool UdpTracker::announce(const std::string &tracker_host,
                          uint16_t tracker_port,
                          const TorrentMeta &meta,
                          int32_t local_port,
                          UdpTrackerHandler handler,
                          int64_t uploaded,
                          int64_t downloaded,
                          int64_t left)
{
    info_hash_ = meta.info_hash_;
    handler_ = handler;
    sync_result_ = nullptr;
    is_sync_ = false;

    pending_uploaded_ = uploaded;
    pending_downloaded_ = downloaded;
    pending_left_ = left;
    pending_local_port_ = local_port;

    if (!setup_connection(tracker_host, tracker_port))
        return false;

    timeout_timer_ = timer_manager_->timeout(DEFAULT_TIMEOUT_MS, this);

    ev_loop_->queue_in_loop([this]()
    {
        send_connect_request();
    });

    ev_loop_->loop();
    return true;
}

} // namespace yuan::net::bit_torrent
