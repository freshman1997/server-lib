#include "http2/session.h"

#include "logger.h"
#include "net/connection/connection.h"

#include <cstring>
#include <string>

namespace yuan::net::http::http2
{
    std::string Session::status_to_reason(std::uint16_t status)
    {
        switch (status) {
        case 200:
            return "200";
        case 204:
            return "204";
        case 400:
            return "400";
        case 404:
            return "404";
        case 500:
            return "500";
        case 505:
            return "505";
        default:
            return std::to_string(status);
        }
    }

    Session::StreamEntry *Session::get_or_create_stream(std::uint32_t stream_id)
    {
        if (stream_id == 0) {
            return nullptr;
        }
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            return &it->second;
        }
        auto [inserted_it, ok] = streams_.emplace(stream_id, StreamEntry{});
        if (!ok) {
            return nullptr;
        }
        inserted_it->second.state = StreamState::open;
        inserted_it->second.recv_window = local_settings_.initial_window_size;
        inserted_it->second.send_window = peer_settings_.initial_window_size;
        if (stream_id > last_stream_id_) {
            last_stream_id_ = stream_id;
        }
        return &inserted_it->second;
    }

    Session::Session(const std::shared_ptr<net::Connection> &conn)
        : conn_(conn)
        , connection_recv_window_(local_settings_.initial_window_size)
        , connection_send_window_(peer_settings_.initial_window_size)
    {
    }

    void Session::send_server_settings()
    {
        if (server_settings_sent_) {
            return;
        }
        server_settings_sent_ = true;

        Frame settings_frame;
        settings_frame.header.type = FrameType::settings;
        settings_frame.header.flags = flag_none;
        settings_frame.header.stream_id = 0;

        auto &p = settings_frame.payload;

        auto append_setting = [&p](SettingsId id, std::uint32_t value) {
            const auto id_val = static_cast<std::uint16_t>(id);
            p.push_back(static_cast<std::uint8_t>((id_val >> 8) & 0xff));
            p.push_back(static_cast<std::uint8_t>(id_val & 0xff));
            p.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
            p.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
            p.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
            p.push_back(static_cast<std::uint8_t>(value & 0xff));
        };

        append_setting(SettingsId::header_table_size, local_settings_.header_table_size);
        if (!local_settings_.enable_push) {
            append_setting(SettingsId::enable_push, 0);
        }
        append_setting(SettingsId::max_concurrent_streams, local_settings_.max_concurrent_streams);
        append_setting(SettingsId::initial_window_size, local_settings_.initial_window_size);
        append_setting(SettingsId::max_frame_size, local_settings_.max_frame_size);
        append_setting(SettingsId::max_header_list_size, local_settings_.max_header_list_size);

        settings_frame.header.length = static_cast<std::uint32_t>(p.size());
        send_frame(settings_frame);
        awaiting_settings_ack_ = true;
    }

    void Session::apply_peer_settings(const Frame &settings_frame)
    {
        const auto &payload = settings_frame.payload;
        if ((payload.size() % 6) != 0) {
            return;
        }

        for (std::size_t i = 0; i + 5 < payload.size(); i += 6) {
            const std::uint16_t id = (static_cast<std::uint16_t>(payload[i]) << 8) |
                                     static_cast<std::uint16_t>(payload[i + 1]);
            const std::uint32_t value = (static_cast<std::uint32_t>(payload[i + 2]) << 24) |
                                        (static_cast<std::uint32_t>(payload[i + 3]) << 16) |
                                        (static_cast<std::uint32_t>(payload[i + 4]) << 8) |
                                        static_cast<std::uint32_t>(payload[i + 5]);

            switch (static_cast<SettingsId>(id)) {
            case SettingsId::header_table_size:
                peer_settings_.header_table_size = value;
                hpack_encoder_.set_max_table_size(value);
                break;
            case SettingsId::enable_push:
                peer_settings_.enable_push = (value != 0);
                break;
            case SettingsId::max_concurrent_streams:
                peer_settings_.max_concurrent_streams = value;
                break;
            case SettingsId::initial_window_size:
                if (value > 2147483647u) {
                    send_frame(FrameCodec::make_goaway(ErrorCode::flow_control_error, last_stream_id_));
                    if (conn_) conn_->close();
                    return;
                }
                peer_settings_.initial_window_size = value;
                break;
            case SettingsId::max_frame_size:
                if (value < 16384 || value > 16777215) {
                    send_frame(FrameCodec::make_goaway(ErrorCode::protocol_error, last_stream_id_));
                    if (conn_) conn_->close();
                    return;
                }
                peer_settings_.max_frame_size = value;
                break;
            case SettingsId::max_header_list_size:
                peer_settings_.max_header_list_size = value;
                break;
            default:
                break;
            }
        }

        LOG_INFO("[HTTP2] peer settings applied: header_table_size={} enable_push={} max_concurrent_streams={} "
                 "initial_window_size={} max_frame_size={} max_header_list_size={}",
                 peer_settings_.header_table_size,
                 peer_settings_.enable_push,
                 peer_settings_.max_concurrent_streams,
                 peer_settings_.initial_window_size,
                 peer_settings_.max_frame_size,
                 peer_settings_.max_header_list_size);
    }

    void Session::send_window_update(std::uint32_t stream_id, std::uint32_t increment)
    {
        if (increment == 0) {
            return;
        }

        Frame wu;
        wu.header.type = FrameType::window_update;
        wu.header.flags = flag_none;
        wu.header.stream_id = stream_id;
        wu.header.length = 4;
        wu.payload.resize(4);
        wu.payload[0] = static_cast<std::uint8_t>((increment >> 24) & 0x7f);
        wu.payload[1] = static_cast<std::uint8_t>((increment >> 16) & 0xff);
        wu.payload[2] = static_cast<std::uint8_t>((increment >> 8) & 0xff);
        wu.payload[3] = static_cast<std::uint8_t>(increment & 0xff);
        send_frame(wu);
    }

    void Session::check_and_send_window_update(std::uint32_t stream_id, std::size_t bytes_consumed)
    {
        if (bytes_consumed == 0) {
            return;
        }

        constexpr std::uint32_t kWindowUpdateThreshold = 32768;

        if (stream_id == 0) {
            if (connection_recv_window_ < local_settings_.initial_window_size / 2 ||
                bytes_consumed >= kWindowUpdateThreshold) {
                const std::uint32_t increment = static_cast<std::uint32_t>(
                    local_settings_.initial_window_size - connection_recv_window_);
                if (increment > 0) {
                    connection_recv_window_ = local_settings_.initial_window_size;
                    send_window_update(0, increment);
                }
            }
            return;
        }

        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }

        auto &stream = it->second;
        if (stream.recv_window < local_settings_.initial_window_size / 2 ||
            bytes_consumed >= kWindowUpdateThreshold) {
            const std::uint32_t increment = static_cast<std::uint32_t>(
                local_settings_.initial_window_size - stream.recv_window);
            if (increment > 0) {
                stream.recv_window = local_settings_.initial_window_size;
                send_window_update(stream_id, increment);
            }
        }
    }

    bool Session::on_preface_received()
    {
        if (!conn_) {
            return false;
        }

        send_server_settings();
        return true;
    }

    bool Session::on_bytes(::yuan::buffer::ByteBuffer &input)
    {
        while (true) {
            auto frame = FrameCodec::try_decode(input, local_settings_.max_frame_size);
            if (!frame.has_value()) {
                break;
            }

            ++frames_received_;
            if (!handle_frame(*frame)) {
                return false;
            }
        }
        return true;
    }

    void Session::send_simple_response(std::uint32_t stream_id,
                                       std::uint16_t status,
                                       std::string_view content_type,
                                       std::string_view body)
    {
        if (!conn_ || stream_id == 0) {
            return;
        }

        std::vector<std::pair<std::string_view, std::string_view>> headers;
        const std::string status_str = std::to_string(status);
        headers.emplace_back(":status", status_str);
        if (!content_type.empty()) {
            headers.emplace_back("content-type", content_type);
        }
        if (!body.empty()) {
            const std::string len_str = std::to_string(body.size());
            headers.emplace_back("content-length", len_str);
        }

        send_headers(stream_id, headers, body.empty());
        if (!body.empty()) {
            send_data(stream_id, body, true);
        }
    }

    void Session::send_headers(std::uint32_t stream_id,
                               const std::vector<std::pair<std::string_view, std::string_view>> &headers,
                               bool end_stream)
    {
        if (!conn_ || stream_id == 0) {
            return;
        }

        std::vector<std::uint8_t> header_block;
        header_block.reserve(64 + headers.size() * 32);
        for (const auto &[name, value] : headers) {
            hpack_encoder_.encode_header(header_block, name, value);
        }

        Frame hdr;
        hdr.header.type = FrameType::headers;
        hdr.header.stream_id = stream_id;
        hdr.header.flags = flag_end_headers;
        if (end_stream) {
            hdr.header.flags |= flag_end_stream;
        }
        hdr.payload = std::move(header_block);
        hdr.header.length = static_cast<std::uint32_t>(hdr.payload.size());
        send_frame(hdr);

        if (end_stream) {
            auto it = streams_.find(stream_id);
            if (it != streams_.end()) {
                if (it->second.state == StreamState::open) {
                    it->second.state = StreamState::half_closed_local;
                } else if (it->second.state == StreamState::half_closed_remote) {
                    it->second.state = StreamState::closed;
                    streams_.erase(it);
                }
            }
        }
    }

    void Session::send_data(std::uint32_t stream_id, const std::uint8_t *data, std::size_t len, bool end_stream)
    {
        if (!conn_ || stream_id == 0) {
            if (end_stream && conn_ && stream_id != 0) {
                Frame empty_data;
                empty_data.header.type = FrameType::data;
                empty_data.header.stream_id = stream_id;
                empty_data.header.flags = flag_end_stream;
                empty_data.header.length = 0;
                send_frame(empty_data);

                auto it = streams_.find(stream_id);
                if (it != streams_.end()) {
                    if (it->second.state == StreamState::open) {
                        it->second.state = StreamState::half_closed_local;
                    } else if (it->second.state == StreamState::half_closed_remote) {
                        it->second.state = StreamState::closed;
                        streams_.erase(it);
                    }
                }
            }
            return;
        }

        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }

        if (it->second.has_pending_write) {
            auto &pw = it->second.pending_write;
            if (data && len > 0) {
                pw.data.insert(pw.data.end(), data, data + len);
            }
            if (end_stream) {
                pw.end_stream = true;
            }
            return;
        }

        if (len == 0) {
            if (end_stream) {
                Frame empty_data;
                empty_data.header.type = FrameType::data;
                empty_data.header.stream_id = stream_id;
                empty_data.header.flags = flag_end_stream;
                empty_data.header.length = 0;
                send_frame(empty_data);

                if (it->second.state == StreamState::open) {
                    it->second.state = StreamState::half_closed_local;
                } else if (it->second.state == StreamState::half_closed_remote) {
                    it->second.state = StreamState::closed;
                    streams_.erase(it);
                }
            }
            return;
        }

        std::size_t offset = 0;
        const std::uint32_t max_chunk = peer_settings_.max_frame_size;

        while (offset < len) {
            const std::uint32_t available_window = (std::min)(connection_send_window_, it->second.send_window);
            if (available_window == 0) {
                auto &pw = it->second.pending_write;
                pw.data.assign(data + offset, data + len);
                pw.offset = 0;
                pw.end_stream = end_stream;
                it->second.has_pending_write = true;
                pending_write_queue_.push_back(stream_id);
                return;
            }

            const std::size_t remaining = len - offset;
            const std::uint32_t chunk_size = static_cast<std::uint32_t>(
                (std::min)({static_cast<std::size_t>(max_chunk), remaining, static_cast<std::size_t>(available_window)}));

            const bool is_last = (offset + chunk_size >= len);
            const bool end_this = is_last && end_stream;

            Frame data_frame;
            data_frame.header.type = FrameType::data;
            data_frame.header.stream_id = stream_id;
            data_frame.header.flags = end_this ? flag_end_stream : flag_none;
            data_frame.payload.assign(data + offset, data + offset + chunk_size);
            data_frame.header.length = chunk_size;
            send_frame(data_frame);

            connection_send_window_ -= chunk_size;
            it->second.send_window -= chunk_size;
            offset += chunk_size;
        }

        if (end_stream) {
            if (it->second.state == StreamState::open) {
                it->second.state = StreamState::half_closed_local;
            } else if (it->second.state == StreamState::half_closed_remote) {
                it->second.state = StreamState::closed;
                streams_.erase(it);
            }
        }
    }

    void Session::send_data(std::uint32_t stream_id, std::string_view body, bool end_stream)
    {
        send_data(stream_id, reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), end_stream);
    }

    void Session::send_rst_stream(std::uint32_t stream_id, ErrorCode error)
    {
        if (stream_id == 0) {
            return;
        }

        Frame rst;
        rst.header.type = FrameType::rst_stream;
        rst.header.flags = flag_none;
        rst.header.stream_id = stream_id;
        rst.header.length = 4;
        rst.payload.resize(4);
        const auto err = static_cast<std::uint32_t>(error);
        rst.payload[0] = static_cast<std::uint8_t>((err >> 24) & 0xff);
        rst.payload[1] = static_cast<std::uint8_t>((err >> 16) & 0xff);
        rst.payload[2] = static_cast<std::uint8_t>((err >> 8) & 0xff);
        rst.payload[3] = static_cast<std::uint8_t>(err & 0xff);
        send_frame(rst);

        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            it->second.state = StreamState::closed;
            streams_.erase(it);
        }
    }

    void Session::send_goaway(ErrorCode error)
    {
        send_frame(FrameCodec::make_goaway(error, last_stream_id_));
        if (goaway_bridge_) {
            goaway_bridge_(error, last_stream_id_);
        }
    }

    void Session::close_gracefully()
    {
        if (!conn_) {
            return;
        }

        send_frame(FrameCodec::make_goaway(ErrorCode::no_error, last_stream_id_));
        pending_write_queue_.clear();
        for (auto &[id, stream] : streams_) {
            stream.has_pending_write = false;
        }
    }

    void Session::flush_pending_writes()
    {
        while (!pending_write_queue_.empty()) {
            const std::uint32_t stream_id = pending_write_queue_.front();
            auto it = streams_.find(stream_id);
            if (it == streams_.end() || !it->second.has_pending_write) {
                pending_write_queue_.pop_front();
                continue;
            }

            auto &stream = it->second;
            auto &pw = stream.pending_write;
            const std::uint32_t max_chunk = peer_settings_.max_frame_size;

            while (pw.offset < pw.data.size()) {
                const std::uint32_t available_window = (std::min)(connection_send_window_, stream.send_window);
                if (available_window == 0) {
                    return;
                }

                const std::size_t remaining = pw.data.size() - pw.offset;
                const std::uint32_t chunk_size = static_cast<std::uint32_t>(
                    (std::min)({static_cast<std::size_t>(max_chunk), remaining, static_cast<std::size_t>(available_window)}));

                const bool is_last = (pw.offset + chunk_size >= pw.data.size());
                const bool end_this = is_last && pw.end_stream;

                Frame data_frame;
                data_frame.header.type = FrameType::data;
                data_frame.header.stream_id = stream_id;
                data_frame.header.flags = end_this ? flag_end_stream : flag_none;
                data_frame.payload.assign(pw.data.begin() + pw.offset, pw.data.begin() + pw.offset + chunk_size);
                data_frame.header.length = chunk_size;
                send_frame(data_frame);

                connection_send_window_ -= chunk_size;
                stream.send_window -= chunk_size;
                pw.offset += chunk_size;
            }

            stream.has_pending_write = false;
            pending_write_queue_.pop_front();

            if (pw.end_stream) {
                if (stream.state == StreamState::open) {
                    stream.state = StreamState::half_closed_local;
                } else if (stream.state == StreamState::half_closed_remote) {
                    stream.state = StreamState::closed;
                    streams_.erase(it);
                }
            }
        }
    }

    bool Session::validate_stream_state(std::uint32_t stream_id, FrameType frame_type)
    {
        if (stream_id == 0) {
            return true;
        }

        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            if (frame_type == FrameType::data || frame_type == FrameType::headers ||
                frame_type == FrameType::continuation) {
                if (stream_id <= last_stream_id_ && stream_id % 2 == 1) {
                    return false;
                }
            }
            return true;
        }

        const auto state = it->second.state;

        switch (frame_type) {
        case FrameType::data:
            if (state == StreamState::half_closed_remote || state == StreamState::closed) {
                send_rst_stream(stream_id, ErrorCode::stream_closed);
                return false;
            }
            break;
        case FrameType::headers:
            if (state == StreamState::half_closed_remote || state == StreamState::closed) {
                send_rst_stream(stream_id, ErrorCode::stream_closed);
                return false;
            }
            break;
        case FrameType::continuation:
            if (state == StreamState::half_closed_remote || state == StreamState::closed) {
                send_rst_stream(stream_id, ErrorCode::stream_closed);
                return false;
            }
            break;
        case FrameType::rst_stream:
            break;
        case FrameType::window_update:
            if (state == StreamState::closed) {
                return false;
            }
            break;
        default:
            break;
        }

        return true;
    }

    bool Session::handle_frame(const Frame &frame)
    {
        if (!conn_) {
            return false;
        }

        auto close_with_goaway = [this](ErrorCode error) {
            send_frame(FrameCodec::make_goaway(error, last_stream_id_));
            conn_->close();
            return false;
        };

        auto maybe_decode_hpack = [this](StreamEntry &stream) -> bool {
            std::vector<HpackHeaderField> decoded_headers;
            if (!hpack_decoder_.decode(stream.header_block, decoded_headers)) {
                std::string hex;
                hex.reserve(stream.header_block.size() * 3);
                for (auto b : stream.header_block) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02x ", b);
                    hex += buf;
                }
                LOG_ERROR("[HTTP2] HPACK decode failed, header_block size={}, hex={}", stream.header_block.size(), hex);
                return false;
            }

            if (decoded_headers.empty()) {
                return true;
            }

            std::size_t header_list_size = 0;
            for (const auto &h : decoded_headers) {
                header_list_size += 32 + h.name.size() + h.value.size();
            }
            if (header_list_size > local_settings_.max_header_list_size) {
                return false;
            }

            if (stream.received_data) {
                for (const auto &h : decoded_headers) {
                    if (!h.name.empty() && h.name.front() == ':') {
                        return false;
                    }
                }
            }

            std::string plain;
            plain.reserve(stream.header_block.size());
            for (const auto &h : decoded_headers) {
                plain += h.name;
                plain += ": ";
                plain += h.value;
                plain += "\r\n";
            }
            stream.header_block.assign(plain.begin(), plain.end());
            return true;
        };

        auto finalize_header_block = [this](std::uint32_t stream_id, StreamEntry &stream, bool end_stream) {
            if (headers_bridge_) {
                headers_bridge_(
                    stream_id,
                    std::string_view(
                        reinterpret_cast<const char *>(stream.header_block.data()),
                        stream.header_block.size()),
                    end_stream);
            }
            stream.header_block.clear();
            stream.waiting_continuation = false;
            if (end_stream) {
                stream.state = StreamState::half_closed_remote;
            }
        };

        if (!seen_non_settings_frame_) {
            LOG_INFO("[HTTP2] First non-preface frame: type={}, stream_id={}, length={}", static_cast<int>(frame.header.type), frame.header.stream_id, frame.header.length);
            if (frame.header.type != FrameType::settings) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if ((frame.header.flags & flag_ack) != 0 && frame.header.length != 0) {
                return close_with_goaway(ErrorCode::frame_size_error);
            }

            if ((frame.header.flags & flag_ack) == 0) {
                apply_peer_settings(frame);
                send_frame(FrameCodec::make_settings_ack());
            } else {
                awaiting_settings_ack_ = false;
                settings_ack_received_ = true;
            }
            seen_non_settings_frame_ = true;
            return true;
        }

        switch (frame.header.type) {
        case FrameType::settings:
            if (frame.header.stream_id != 0) {
                return close_with_goaway(ErrorCode::protocol_error);
            }
            if ((frame.header.flags & flag_ack) == 0) {
                if ((frame.header.length % 6) != 0) {
                    return close_with_goaway(ErrorCode::frame_size_error);
                }
                apply_peer_settings(frame);
                send_frame(FrameCodec::make_settings_ack());
            } else {
                if (frame.header.length != 0) {
                    return close_with_goaway(ErrorCode::frame_size_error);
                }
                awaiting_settings_ack_ = false;
                settings_ack_received_ = true;
            }
            return true;
        case FrameType::ping:
            if (frame.header.length != 8 || frame.header.stream_id != 0) {
                return close_with_goaway(ErrorCode::frame_size_error);
            }
            if ((frame.header.flags & flag_ack) == 0) {
                send_frame(FrameCodec::make_ping_ack(frame));
            }
            return true;
        case FrameType::priority: {
            if (frame.header.length != 5 || frame.header.stream_id == 0) {
                return close_with_goaway(ErrorCode::protocol_error);
            }
            if (frame.payload.size() < 5) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            auto it = streams_.find(frame.header.stream_id);
            StreamEntry *stream = nullptr;
            if (it != streams_.end()) {
                stream = &it->second;
            } else {
                auto [ins_it, ok] = streams_.emplace(frame.header.stream_id, StreamEntry{});
                if (!ok) {
                    return close_with_goaway(ErrorCode::internal_error);
                }
                stream = &ins_it->second;
            }

            const std::uint32_t raw_dep = (static_cast<std::uint32_t>(frame.payload[0]) << 24) |
                                           (static_cast<std::uint32_t>(frame.payload[1]) << 16) |
                                           (static_cast<std::uint32_t>(frame.payload[2]) << 8) |
                                           static_cast<std::uint32_t>(frame.payload[3]);
            stream->exclusive = (raw_dep & 0x80000000u) != 0;
            stream->parent_stream_id = raw_dep & 0x7fffffffu;
            stream->weight = frame.payload[4] + 1;
            return true;
        }
        case FrameType::headers: {
            LOG_INFO("[HTTP2] HEADERS frame: stream_id={}, length={}, flags=0x{:02x}", frame.header.stream_id, frame.header.length, frame.header.flags);
            if (frame.header.stream_id == 0) {
                LOG_ERROR("[HTTP2] HEADERS stream_id=0");
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if ((frame.header.stream_id & 1) == 0) {
                LOG_ERROR("[HTTP2] HEADERS even stream_id={}", frame.header.stream_id);
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if (frame.header.stream_id <= last_stream_id_ &&
                streams_.find(frame.header.stream_id) == streams_.end()) {
                LOG_ERROR("[HTTP2] HEADERS stream_id={} <= last_stream_id_={} and not found", frame.header.stream_id, last_stream_id_);
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if (!validate_stream_state(frame.header.stream_id, FrameType::headers)) {
                LOG_ERROR("[HTTP2] HEADERS validate_stream_state failed for stream_id={}", frame.header.stream_id);
                return true;
            }

            if (open_stream_count() >= local_settings_.max_concurrent_streams) {
                auto it = streams_.find(frame.header.stream_id);
                if (it == streams_.end()) {
                    Frame rst;
                    rst.header.type = FrameType::rst_stream;
                    rst.header.flags = flag_none;
                    rst.header.stream_id = frame.header.stream_id;
                    rst.header.length = 4;
                    rst.payload.resize(4);
                    const auto err = static_cast<std::uint32_t>(ErrorCode::refused_stream);
                    rst.payload[0] = static_cast<std::uint8_t>((err >> 24) & 0xff);
                    rst.payload[1] = static_cast<std::uint8_t>((err >> 16) & 0xff);
                    rst.payload[2] = static_cast<std::uint8_t>((err >> 8) & 0xff);
                    rst.payload[3] = static_cast<std::uint8_t>(err & 0xff);
                    send_frame(rst);
                    return true;
                }
            }

            auto *stream = get_or_create_stream(frame.header.stream_id);
            if (!stream) {
                return close_with_goaway(ErrorCode::internal_error);
            }

            if (stream->waiting_continuation) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            const bool end_stream = (frame.header.flags & flag_end_stream) != 0;
            const bool end_headers = (frame.header.flags & flag_end_headers) != 0;

            stream->header_block = frame.payload;
            stream->headers_end_stream = end_stream;
            stream->continuation_count = 0;

            if (stream->header_block.size() > Session::kMaxHeaderBlockSize) {
                send_goaway(ErrorCode::enhance_your_calm);
                conn_->close();
                return false;
            }

            if (end_headers) {
                if (!maybe_decode_hpack(*stream)) {
                    LOG_ERROR("[HTTP2] HPACK decode failed for stream_id={}", frame.header.stream_id);
                    send_goaway(ErrorCode::protocol_error);
                    conn_->close();
                    return false;
                }
                LOG_INFO("[HTTP2] HPACK decode OK for stream_id={}", frame.header.stream_id);
                finalize_header_block(frame.header.stream_id, *stream, end_stream);
            } else {
                stream->waiting_continuation = true;
            }
            return true;
        }
        case FrameType::data: {
            if (frame.header.stream_id == 0) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if (!validate_stream_state(frame.header.stream_id, FrameType::data)) {
                return true;
            }

            auto it = streams_.find(frame.header.stream_id);
            if (it == streams_.end()) {
                return close_with_goaway(ErrorCode::protocol_error);
            }
            auto *stream = &it->second;

            if (stream->waiting_continuation) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            const bool end_stream = (frame.header.flags & flag_end_stream) != 0;
            const auto data_size = frame.payload.size();
            stream->received_bytes += data_size;
            stream->received_data = true;

            if (data_size > connection_recv_window_) {
                return close_with_goaway(ErrorCode::flow_control_error);
            }
            connection_recv_window_ -= static_cast<std::uint32_t>(data_size);

            if (data_size > stream->recv_window) {
                return close_with_goaway(ErrorCode::flow_control_error);
            }
            stream->recv_window -= static_cast<std::uint32_t>(data_size);

            if (data_bridge_) {
                data_bridge_(frame.header.stream_id, frame.payload, end_stream);
            }

            check_and_send_window_update(0, data_size);
            check_and_send_window_update(frame.header.stream_id, data_size);

            if (end_stream) {
                stream->state = StreamState::half_closed_remote;
            }
            return true;
        }
        case FrameType::continuation: {
            if (frame.header.stream_id == 0) {
                return close_with_goaway(ErrorCode::protocol_error);
            }

            if (!validate_stream_state(frame.header.stream_id, FrameType::continuation)) {
                return true;
            }

            auto it = streams_.find(frame.header.stream_id);
            if (it == streams_.end() || !it->second.waiting_continuation) {
                return close_with_goaway(ErrorCode::protocol_error);
            }
            auto *stream = &it->second;

            stream->header_block.insert(stream->header_block.end(), frame.payload.begin(), frame.payload.end());

            ++stream->continuation_count;
            if (stream->continuation_count > Session::kMaxContinuationFrames) {
                send_goaway(ErrorCode::enhance_your_calm);
                conn_->close();
                return false;
            }

            if (stream->header_block.size() > Session::kMaxHeaderBlockSize) {
                send_goaway(ErrorCode::enhance_your_calm);
                conn_->close();
                return false;
            }

            const bool end_headers = (frame.header.flags & flag_end_headers) != 0;
            if (end_headers) {
                if (!maybe_decode_hpack(*stream)) {
                    send_goaway(ErrorCode::protocol_error);
                    conn_->close();
                    return false;
                }
                finalize_header_block(frame.header.stream_id, *stream, stream->headers_end_stream);
            }
            return true;
        }
        case FrameType::window_update: {
            if (frame.header.length != 4) {
                return close_with_goaway(ErrorCode::frame_size_error);
            }
            if (frame.payload.size() >= 4) {
                const std::uint32_t increment =
                    ((static_cast<std::uint32_t>(frame.payload[0]) & 0x7f) << 24) |
                    (static_cast<std::uint32_t>(frame.payload[1]) << 16) |
                    (static_cast<std::uint32_t>(frame.payload[2]) << 8) |
                    static_cast<std::uint32_t>(frame.payload[3]);

                if (increment == 0) {
                    if (frame.header.stream_id == 0) {
                        return close_with_goaway(ErrorCode::protocol_error);
                    }
                    Frame rst;
                    rst.header.type = FrameType::rst_stream;
                    rst.header.flags = flag_none;
                    rst.header.stream_id = frame.header.stream_id;
                    rst.header.length = 4;
                    rst.payload.resize(4);
                    const auto err = static_cast<std::uint32_t>(ErrorCode::protocol_error);
                    rst.payload[0] = static_cast<std::uint8_t>((err >> 24) & 0xff);
                    rst.payload[1] = static_cast<std::uint8_t>((err >> 16) & 0xff);
                    rst.payload[2] = static_cast<std::uint8_t>((err >> 8) & 0xff);
                    rst.payload[3] = static_cast<std::uint8_t>(err & 0xff);
                    send_frame(rst);
                    return true;
                }

                if (frame.header.stream_id == 0) {
                    const auto old_window = connection_send_window_;
                    connection_send_window_ += increment;
                    if (connection_send_window_ < old_window) {
                        return close_with_goaway(ErrorCode::flow_control_error);
                    }
                    flush_pending_writes();
                } else {
                    auto it = streams_.find(frame.header.stream_id);
                    if (it != streams_.end()) {
                        const auto old_window = it->second.send_window;
                        it->second.send_window += increment;
                        if (it->second.send_window < old_window) {
                            send_rst_stream(frame.header.stream_id, ErrorCode::flow_control_error);
                            return true;
                        }
                        flush_pending_writes();
                    }
                }
            }
            return true;
        }
        case FrameType::rst_stream:
            if (frame.header.length != 4 || frame.header.stream_id == 0) {
                return close_with_goaway(ErrorCode::protocol_error);
            }
            {
                auto it = streams_.find(frame.header.stream_id);
                if (it != streams_.end()) {
                    it->second.state = StreamState::closed;
                    it->second.has_pending_write = false;
                    streams_.erase(it);
                }
            }
            return true;
        case FrameType::goaway:
            conn_->close();
            return false;
        default:
            return true;
        }
    }

    void Session::send_frame(const Frame &frame)
    {
        if (!conn_) {
            return;
        }

        ::yuan::buffer::ByteBuffer out;
        FrameCodec::encode_frame(frame, out);
        conn_->write_and_flush(out);
    }
}
