#include "tracker/udp_tracker.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "utils.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/datagram_transport.h"
#include "net/socket/socket.h"
#include "net/connection/connection.h"
#include "net/poller/select_poller.h"
#include "timer/timer_util.hpp"
#include "timer/wheel_timer_manager.h"
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
    completion_event_.reset();
    std::random_device rd;
    next_tid_ = rd();
}

UdpTracker::~UdpTracker() { disconnect(); }

void UdpTracker::disconnect()
{
    waiting_response_ = false;
    connected_to_tracker_ = false;
    coroutine_mode_ = false;

    if (timeout_timer_)
    {
        timeout_timer_->cancel();
        timeout_timer_ = nullptr;
    }

    connection_ = nullptr;

    if (acceptor_)
    {
        delete acceptor_;
        acceptor_ = nullptr;
    }

    if (own_loop_)
    {
        if (ev_loop_)
        {
            ev_loop_->quit();
            delete ev_loop_;
        }
        if (poller_)
        {
            delete poller_;
        }
        ev_loop_ = nullptr;
        poller_ = nullptr;
        if (timer_manager_)
        {
            delete timer_manager_;
            timer_manager_ = nullptr;
        }
    }

    ev_loop_ = nullptr;
    timer_manager_ = nullptr;
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
        if (handler_)
            handler_(err_resp);
        else if (sync_result_)
            *sync_result_ = err_resp;
        completion_event_.notify();
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
        if (handler_)
            handler_(err_resp);
        else if (sync_result_)
            *sync_result_ = err_resp;
        completion_event_.notify();
    }
}

void UdpTracker::on_read(net::Connection *conn)
{
    auto byte_buffer = conn->take_input_byte_buffer();
    if (byte_buffer.readable_bytes() < 4) return;

    const char *data = byte_buffer.read_ptr();
    int32_t action = (static_cast<uint8_t>(data[0]) << 24) |
                     (static_cast<uint8_t>(data[1]) << 16) |
                     (static_cast<uint8_t>(data[2]) << 8) |
                     static_cast<uint8_t>(data[3]);

    switch (action)
    {
    case CONNECT_ACTION:
        if (byte_buffer.readable_bytes() >= 16)
            handle_connect_response(byte_buffer);
        break;
    case ANNOUNCE_ACTION:
        if (byte_buffer.readable_bytes() >= 20)
            handle_announce_response(byte_buffer);
        break;
    case ERROR_ACTION:
        handle_error_response(byte_buffer);
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

    yuan::buffer::ByteBuffer buffer(16);
    // connection_id (8 bytes, big-endian)
    for (int i = 7; i >= 0; i--)
    {
        buffer.append_u8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
    }
    buffer.append_i32(0); // action = connect
    buffer.append_i32(static_cast<int32_t>(transaction_id_));

    if (connection_)
        connection_->write_and_flush(buffer);
}

void UdpTracker::send_announce_request(int64_t uploaded, int64_t downloaded, int64_t left, int32_t local_port, TrackerAnnounceEvent event)
{
    transaction_id_ = next_transaction_id();
    expected_action_ = ANNOUNCE_ACTION;
    waiting_response_ = true;

    yuan::buffer::ByteBuffer buffer(98);
    // connection_id (8 bytes, big-endian)
    for (int i = 7; i >= 0; i--)
    {
        buffer.append_u8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
    }
    buffer.append_i32(1); // action = announce
    buffer.append_i32(static_cast<int32_t>(transaction_id_));
    buffer.append(reinterpret_cast<const char *>(info_hash_.data()), 20);
    buffer.append(peer_id_.data(), 20);
    buffer.append_i64(downloaded);
    buffer.append_i64(left);
    buffer.append_i64(uploaded);
    int32_t event_code = 0;
    switch (event)
    {
    case TrackerAnnounceEvent::completed: event_code = 1; break;
    case TrackerAnnounceEvent::started: event_code = 2; break;
    case TrackerAnnounceEvent::stopped: event_code = 3; break;
    case TrackerAnnounceEvent::none:
    default: event_code = 0; break;
    }
    buffer.append_i32(event_code);
    buffer.append_i32(0); // IP = 0
    buffer.append_i32(static_cast<int32_t>(next_tid_)); // key
    buffer.append_i32(-1); // num_want = as many as possible
    buffer.append_u16(static_cast<uint16_t>(local_port));

    if (connection_)
        connection_->write_and_flush(buffer);
}

void UdpTracker::handle_connect_response(yuan::buffer::ByteBuffer &buf)
{
    if (buf.readable_bytes() < 16) return;

    int32_t action = buf.read_i32();
    int32_t tid = buf.read_i32();

    if (action != CONNECT_ACTION || tid != static_cast<int32_t>(transaction_id_))
        return;

    // Read connection_id (8 bytes, big-endian)
    connection_id_ = 0;
    for (int i = 0; i < 8; i++)
    {
        connection_id_ = (connection_id_ << 8) | static_cast<uint8_t>(buf.read_i8());
    }

    connected_to_tracker_ = true;
    waiting_response_ = false;

    send_announce_request(pending_uploaded_, pending_downloaded_, pending_left_, pending_local_port_, pending_event_);
}

void UdpTracker::handle_announce_response(yuan::buffer::ByteBuffer &buf)
{
    if (buf.readable_bytes() < 20) return;

    int32_t action = buf.read_i32();
    int32_t tid = buf.read_i32();

    if (action != ANNOUNCE_ACTION || tid != static_cast<int32_t>(transaction_id_))
        return;

    UdpTrackerResponse resp;
    resp.interval_ = buf.read_i32();
    resp.incomplete_ = buf.read_i32();
    resp.complete_ = buf.read_i32();

    size_t remaining = buf.readable_bytes();
    size_t peer_count = remaining / 6;

    for (size_t i = 0; i < peer_count; i++)
    {
        PeerAddress addr;
        uint8_t b0 = static_cast<uint8_t>(buf.read_i8());
        uint8_t b1 = static_cast<uint8_t>(buf.read_i8());
        uint8_t b2 = static_cast<uint8_t>(buf.read_i8());
        uint8_t b3 = static_cast<uint8_t>(buf.read_i8());
        addr.ip_ = std::to_string(b0) + "." + std::to_string(b1) + "." +
                   std::to_string(b2) + "." + std::to_string(b3);
        addr.port_ = buf.read_u16();
        resp.peers_.push_back(addr);
    }

    waiting_response_ = false;

    if (handler_)
        handler_(resp);
    else if (sync_result_)
        *sync_result_ = resp;
    completion_event_.notify();
}

void UdpTracker::handle_error_response(yuan::buffer::ByteBuffer &buf)
{
    if (buf.readable_bytes() < 8) return;

    int32_t action = buf.read_i32();
    int32_t tid = buf.read_i32();

    std::string error_msg(buf.read_ptr(), buf.readable_bytes());

    waiting_response_ = false;

    UdpTrackerResponse resp;
    resp.is_error = true;
    resp.error_message_ = error_msg;

    if (handler_)
        handler_(resp);
    else if (sync_result_)
        *sync_result_ = resp;
    completion_event_.notify();
}

bool UdpTracker::init_runtime(timer::TimerManager *timer_manager, net::EventLoop *ev_loop)
{
    own_loop_ = (timer_manager == nullptr || ev_loop == nullptr);

    if (own_loop_)
    {
        poller_ = new net::SelectPoller();
        timer_manager_ = new timer::WheelTimerManager();
        ev_loop_ = new net::EventLoop(poller_, timer_manager_);
        return poller_ && timer_manager_ && ev_loop_;
    }

    poller_ = nullptr;
    timer_manager_ = timer_manager;
    ev_loop_ = ev_loop;
    return timer_manager_ && ev_loop_;
}

// Internal helper: setup socket, acceptor, event loop, and UDP connection
bool UdpTracker::setup_connection(const std::string &tracker_host, uint16_t tracker_port)
{
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

    acceptor_ = net::create_datagram_acceptor(sock, timer_manager_);
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

    auto *conn = res.second;
    auto *datagram = dynamic_cast<net::DatagramTransport *>(conn);
    if (!datagram)
    {
        disconnect();
        return false;
    }

    conn->set_connection_handler(this);
    conn->set_event_handler(ev_loop_);
    datagram->attach_datagram_instance(instance);
    datagram->set_datagram_state(net::ConnectionState::connected);
    instance->enable_rw_events();
    connection_ = conn;

    return true;
}

yuan::coroutine::Task<UdpTrackerResponse> UdpTracker::announce_async(
    yuan::coroutine::RuntimeView runtime,
    const std::string &tracker_host,
    uint16_t tracker_port,
    const TorrentMeta &meta,
    int32_t local_port,
    int64_t uploaded,
    int64_t downloaded,
    int64_t left,
    TrackerAnnounceEvent event)
{
    UdpTrackerResponse response;

    info_hash_ = meta.info_hash_;
    handler_ = nullptr;
    sync_result_ = &response;
    is_sync_ = true;
    coroutine_mode_ = true;

    pending_uploaded_ = uploaded;
    pending_downloaded_ = downloaded;
    pending_left_ = left;
    pending_local_port_ = local_port;
    pending_event_ = event;

    if (!init_runtime(runtime.timer_manager(), runtime.event_loop()))
    {
        sync_result_ = nullptr;
        coroutine_mode_ = false;
        co_return UdpTrackerResponse();
    }

    if (!setup_connection(tracker_host, tracker_port))
    {
        disconnect();
        sync_result_ = nullptr;
        coroutine_mode_ = false;
        co_return UdpTrackerResponse();
    }

    timeout_timer_ = timer::TimerUtil::build_timeout_timer(
        timer_manager_, DEFAULT_TIMEOUT_MS, this, &UdpTracker::on_timer);

    completion_event_.reset(runtime.event_loop());
    co_await runtime.dispatch_in_loop([this]() {
        send_connect_request();
    });

    const bool timed_out = co_await completion_event_.wait_for(runtime.timer_manager(), DEFAULT_TIMEOUT_MS);
    if (timed_out && !response.is_error)
    {
        response.is_error = true;
        response.error_message_ = "timeout";
    }
    const auto result = response;

    disconnect();
    sync_result_ = nullptr;
    coroutine_mode_ = false;
    co_return result;
}

bool UdpTracker::announce(const std::string &tracker_host,
                          uint16_t tracker_port,
                          const TorrentMeta &meta,
                          int32_t local_port,
                          int64_t uploaded,
                          int64_t downloaded,
                          int64_t left,
                          TrackerAnnounceEvent event,
                          UdpTrackerResponse *out)
{
    net::SelectPoller poller;
    timer::WheelTimerManager timer_manager;
    net::EventLoop loop(&poller, &timer_manager);
    yuan::coroutine::RuntimeView runtime(&loop, &timer_manager);
    const auto response = yuan::coroutine::sync_wait(
        runtime,
        announce_async(runtime, tracker_host, tracker_port, meta, local_port, uploaded, downloaded, left, event));
    if (out)
    {
        *out = response;
    }
    return !response.is_error;
}

bool UdpTracker::announce(const std::string &tracker_host,
                          uint16_t tracker_port,
                          const TorrentMeta &meta,
                          int32_t local_port,
                          UdpTrackerHandler handler,
                          int64_t uploaded,
                          int64_t downloaded,
                          int64_t left,
                          TrackerAnnounceEvent event)
{
    if (!handler)
    {
        return false;
    }

    net::SelectPoller poller;
    timer::WheelTimerManager timer_manager;
    net::EventLoop loop(&poller, &timer_manager);
    yuan::coroutine::RuntimeView runtime(&loop, &timer_manager);
    const auto response = yuan::coroutine::sync_wait(
        runtime,
        announce_async(runtime, tracker_host, tracker_port, meta, local_port, uploaded, downloaded, left, event));
    handler(response);
    return true;
}

} // namespace yuan::net::bit_torrent
