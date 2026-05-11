#include "tracker/udp_tracker.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime.h"
#include "coroutine/sync_wait.h"
#include "utils.h"
#include "net/socket/socket.h"
#include "net/runtime/network_runtime.h"
#include <cstring>
#include <random>

namespace yuan::net::bit_torrent
{

    UdpTracker::UdpTracker()
        : peer_id_(generate_peer_id())
    {
        std::random_device rd;
        next_tid_ = rd();
    }

    UdpTracker::~UdpTracker()
    {
        disconnect();
    }

    void UdpTracker::disconnect()
    {
        client_.close();
        if (owned_runtime_) {
            owned_runtime_->stop();
            owned_runtime_.reset();
        }
    }

    uint32_t UdpTracker::next_transaction_id()
    {
        return ++next_tid_;
    }

    yuan::buffer::ByteBuffer UdpTracker::build_connect_request()
    {
        connection_id_ = MAGIC_CONNECTION_ID;
        transaction_id_ = next_transaction_id();

        yuan::buffer::ByteBuffer buffer(16);
        for (int i = 7; i >= 0; i--) {
            buffer.append_u8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
        }
        buffer.append_i32(0);
        buffer.append_i32(static_cast<int32_t>(transaction_id_));
        return buffer;
    }

    yuan::buffer::ByteBuffer UdpTracker::build_announce_request(
        int64_t uploaded, int64_t downloaded, int64_t left,
        int32_t local_port, TrackerAnnounceEvent event,
        const std::string &peer_id)
    {
        transaction_id_ = next_transaction_id();

        yuan::buffer::ByteBuffer buffer(98);
        for (int i = 7; i >= 0; i--) {
            buffer.append_u8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
        }
        buffer.append_i32(1);
        buffer.append_i32(static_cast<int32_t>(transaction_id_));
        buffer.append(reinterpret_cast<const char *>(info_hash_.data()), 20);
        const auto &announce_peer_id = peer_id.size() == 20 ? peer_id : peer_id_;
        buffer.append(announce_peer_id.data(), 20);
        buffer.append_i64(downloaded);
        buffer.append_i64(left);
        buffer.append_i64(uploaded);

        int32_t event_code = 0;
        switch (event) {
        case TrackerAnnounceEvent::completed:
            event_code = 1;
            break;
        case TrackerAnnounceEvent::started:
            event_code = 2;
            break;
        case TrackerAnnounceEvent::stopped:
            event_code = 3;
            break;
        case TrackerAnnounceEvent::none:
        default:
            event_code = 0;
            break;
        }
        buffer.append_i32(event_code);
        buffer.append_i32(0);
        buffer.append_i32(static_cast<int32_t>(next_tid_));
        buffer.append_i32(-1);
        buffer.append_u16(static_cast<uint16_t>(local_port));
        return buffer;
    }

    bool UdpTracker::parse_connect_response(yuan::buffer::ByteBuffer & buf)
    {
        if (buf.readable_bytes() < 16)
            return false;

        int32_t action = buf.read_i32();
        int32_t tid = buf.read_i32();

        if (action != CONNECT_ACTION || tid != static_cast<int32_t>(transaction_id_))
            return false;

        connection_id_ = 0;
        for (int i = 0; i < 8; i++) {
            connection_id_ = (connection_id_ << 8) | static_cast<uint8_t>(buf.read_i8());
        }
        return true;
    }

    UdpTrackerResponse UdpTracker::parse_announce_response(yuan::buffer::ByteBuffer & buf)
    {
        UdpTrackerResponse resp;
        if (buf.readable_bytes() < 20) {
            resp.is_error = true;
            resp.error_message_ = "announce response too short";
            return resp;
        }

        int32_t action = buf.read_i32();
        int32_t tid = buf.read_i32();

        if (action != ANNOUNCE_ACTION || tid != static_cast<int32_t>(transaction_id_)) {
            resp.is_error = true;
            resp.error_message_ = "transaction id mismatch";
            return resp;
        }

        resp.interval_ = buf.read_i32();
        resp.incomplete_ = buf.read_i32();
        resp.complete_ = buf.read_i32();

        size_t remaining = buf.readable_bytes();
        size_t peer_count = remaining / 6;

        for (size_t i = 0; i < peer_count; i++) {
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
        return resp;
    }

    UdpTrackerResponse UdpTracker::parse_error_response(yuan::buffer::ByteBuffer & buf)
    {
        UdpTrackerResponse resp;
        if (buf.readable_bytes() < 8) {
            resp.is_error = true;
            resp.error_message_ = "unknown error";
            return resp;
        }

        buf.read_i32();
        buf.read_i32();

        resp.is_error = true;
        resp.error_message_ = std::string(buf.read_ptr(), buf.readable_bytes());
        return resp;
    }

    yuan::buffer::ByteBuffer UdpTracker::build_scrape_request()
    {
        transaction_id_ = next_transaction_id();

        yuan::buffer::ByteBuffer buffer(36);
        for (int i = 7; i >= 0; i--) {
            buffer.append_u8(static_cast<uint8_t>((connection_id_ >> (i * 8)) & 0xFF));
        }
        buffer.append_i32(SCRAPE_ACTION);
        buffer.append_i32(static_cast<int32_t>(transaction_id_));
        buffer.append(reinterpret_cast<const char *>(info_hash_.data()), 20);
        return buffer;
    }

    UdpScrapeResponse UdpTracker::parse_scrape_response(yuan::buffer::ByteBuffer & buf)
    {
        UdpScrapeResponse resp;
        if (buf.readable_bytes() < 20) {
            resp.is_error = true;
            resp.error_message_ = "scrape response too short";
            return resp;
        }

        int32_t action = buf.read_i32();
        int32_t tid = buf.read_i32();

        if (action != SCRAPE_ACTION || tid != static_cast<int32_t>(transaction_id_)) {
            resp.is_error = true;
            resp.error_message_ = "scrape transaction id mismatch";
            return resp;
        }

        resp.complete_ = buf.read_i32();
        resp.downloaded_ = buf.read_i32();
        resp.incomplete_ = buf.read_i32();
        return resp;
    }

    UdpTrackerResponse UdpTracker::parse_response(yuan::buffer::ByteBuffer & buf)
    {
        if (buf.readable_bytes() < 4) {
            UdpTrackerResponse resp;
            resp.is_error = true;
            resp.error_message_ = "response too short";
            return resp;
        }

        const char *data = buf.read_ptr();
        int32_t action = (static_cast<uint8_t>(data[0]) << 24) |
                         (static_cast<uint8_t>(data[1]) << 16) |
                         (static_cast<uint8_t>(data[2]) << 8) |
                         static_cast<uint8_t>(data[3]);

        switch (action) {
        case CONNECT_ACTION:
            if (parse_connect_response(buf)) {
                UdpTrackerResponse resp;
                return resp;
            }
            {
                UdpTrackerResponse resp;
                resp.is_error = true;
                resp.error_message_ = "connect response parse failed";
                return resp;
            }
        case ANNOUNCE_ACTION:
            return parse_announce_response(buf);
        case SCRAPE_ACTION: {
            UdpTrackerResponse resp;
            auto scrape_resp = parse_scrape_response(buf);
            resp.is_error = scrape_resp.is_error;
            resp.error_message_ = scrape_resp.error_message_;
            return resp;
        }
        case ERROR_ACTION:
            return parse_error_response(buf);
        default: {
            UdpTrackerResponse resp;
            resp.is_error = true;
            resp.error_message_ = "unknown action";
            return resp;
        }
        }
    }

    yuan::coroutine::Task<UdpTrackerResponse> UdpTracker::announce_async(
        yuan::coroutine::RuntimeView runtime,
        const std::string & tracker_host,
        uint16_t tracker_port,
        const TorrentMeta & meta,
        int32_t local_port,
        int64_t uploaded,
        int64_t downloaded,
        int64_t left,
        TrackerAnnounceEvent event,
        const std::string &peer_id)
    {
        UdpTrackerResponse error_resp;

        if (!client_.connect(tracker_host, tracker_port, runtime)) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to setup datagram session";
            client_.close();
            co_return error_resp;
        }

        info_hash_ = meta.info_hash_;

        auto connect_send = client_.send(build_connect_request());
        if (connect_send.status != coroutine::IoStatus::success) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to send connect request";
            client_.close();
            co_return error_resp;
        }

        auto connect_result = co_await client_.receive_async(DEFAULT_TIMEOUT_MS);
        if (connect_result.status != coroutine::IoStatus::success || connect_result.data.readable_bytes() == 0) {
            error_resp.is_error = true;
            error_resp.error_message_ = "connect request timed out";
            client_.close();
            co_return error_resp;
        }

        if (!parse_connect_response(connect_result.data)) {
            error_resp.is_error = true;
            error_resp.error_message_ = "connect response parse failed";
            client_.close();
            co_return error_resp;
        }

        auto announce_send = client_.send(build_announce_request(uploaded, downloaded, left, local_port, event, peer_id));
        if (announce_send.status != coroutine::IoStatus::success) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to send announce request";
            client_.close();
            co_return error_resp;
        }

        auto announce_result = co_await client_.receive_async(DEFAULT_TIMEOUT_MS);
        if (announce_result.status != coroutine::IoStatus::success || announce_result.data.readable_bytes() == 0) {
            error_resp.is_error = true;
            error_resp.error_message_ = "announce request timed out";
            client_.close();
            co_return error_resp;
        }

        auto result = parse_response(announce_result.data);
        client_.close();
        co_return result;
    }

    bool UdpTracker::announce(const std::string & tracker_host,
                              uint16_t tracker_port,
                              const TorrentMeta & meta,
                              int32_t local_port,
                              int64_t uploaded,
                              int64_t downloaded,
                              int64_t left,
                              TrackerAnnounceEvent event,
                              UdpTrackerResponse * out,
                              const std::string &peer_id)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        auto runtime = owned_runtime_->runtime_view();

        auto response = yuan::coroutine::sync_wait(
            runtime,
            announce_async(runtime, tracker_host, tracker_port, meta, local_port,
                           uploaded, downloaded, left, event, peer_id));

        owned_runtime_->stop();

        if (out) {
            *out = response;
        }
        return !response.is_error;
    }

    bool UdpTracker::announce(const std::string & tracker_host,
                              uint16_t tracker_port,
                              const TorrentMeta & meta,
                              int32_t local_port,
                              UdpTrackerHandler handler,
                              int64_t uploaded,
                              int64_t downloaded,
                              int64_t left,
                              TrackerAnnounceEvent event)
    {
        if (!handler) {
            return false;
        }

        UdpTrackerResponse response;
        bool ok = announce(tracker_host, tracker_port, meta, local_port,
                           uploaded, downloaded, left, event, &response);
        handler(response);
        return ok;
    }

    yuan::coroutine::Task<UdpScrapeResponse> UdpTracker::scrape_async(
        yuan::coroutine::RuntimeView runtime,
        const std::string & tracker_host,
        uint16_t tracker_port,
        const TorrentMeta & meta)
    {
        UdpScrapeResponse error_resp;

        if (!client_.connect(tracker_host, tracker_port, runtime)) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to setup datagram session";
            client_.close();
            co_return error_resp;
        }

        info_hash_ = meta.info_hash_;

        auto connect_send = client_.send(build_connect_request());
        if (connect_send.status != coroutine::IoStatus::success) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to send connect request";
            client_.close();
            co_return error_resp;
        }

        auto connect_result = co_await client_.receive_async(DEFAULT_TIMEOUT_MS);
        if (connect_result.status != coroutine::IoStatus::success || connect_result.data.readable_bytes() == 0) {
            error_resp.is_error = true;
            error_resp.error_message_ = "connect request timed out";
            client_.close();
            co_return error_resp;
        }

        if (!parse_connect_response(connect_result.data)) {
            error_resp.is_error = true;
            error_resp.error_message_ = "connect response parse failed";
            client_.close();
            co_return error_resp;
        }

        auto scrape_send = client_.send(build_scrape_request());
        if (scrape_send.status != coroutine::IoStatus::success) {
            error_resp.is_error = true;
            error_resp.error_message_ = "failed to send scrape request";
            client_.close();
            co_return error_resp;
        }

        auto scrape_result = co_await client_.receive_async(DEFAULT_TIMEOUT_MS);
        if (scrape_result.status != coroutine::IoStatus::success || scrape_result.data.readable_bytes() == 0) {
            error_resp.is_error = true;
            error_resp.error_message_ = "scrape request timed out";
            client_.close();
            co_return error_resp;
        }

        auto resp = parse_scrape_response(scrape_result.data);
        client_.close();
        co_return resp;
    }

    bool UdpTracker::scrape(const std::string & tracker_host,
                            uint16_t tracker_port,
                            const TorrentMeta & meta,
                            UdpScrapeResponse * out)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        auto runtime = owned_runtime_->runtime_view();

        auto response = yuan::coroutine::sync_wait(
            runtime,
            scrape_async(runtime, tracker_host, tracker_port, meta));

        owned_runtime_->stop();

        if (out) {
            *out = response;
        }
        return !response.is_error;
    }

    bool UdpTracker::scrape(const std::string & tracker_host,
                            uint16_t tracker_port,
                            const TorrentMeta & meta,
                            UdpScrapeHandler handler)
    {
        if (!handler) {
            return false;
        }

        UdpScrapeResponse response;
        bool ok = scrape(tracker_host, tracker_port, meta, &response);
        handler(response);
        return ok;
    }

} // namespace yuan::net::bit_torrent
