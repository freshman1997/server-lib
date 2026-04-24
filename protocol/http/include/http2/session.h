#ifndef __NET_HTTP2_SESSION_H__
#define __NET_HTTP2_SESSION_H__

#include "buffer/byte_buffer.h"
#include "http2/frame_codec.h"
#include "http2/hpack_decoder.h"
#include "http2/hpack_encoder.h"
#include "http2/types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace yuan::net
{
    class Connection;
}

namespace yuan::net::http::http2
{
    enum class SettingsId : std::uint16_t
    {
        header_table_size = 0x1,
        enable_push = 0x2,
        max_concurrent_streams = 0x3,
        initial_window_size = 0x4,
        max_frame_size = 0x5,
        max_header_list_size = 0x6
    };

    struct PeerSettings
    {
        std::uint32_t header_table_size = 4096;
        bool enable_push = true;
        std::uint32_t max_concurrent_streams = 0xffffffff;
        std::uint32_t initial_window_size = 65535;
        std::uint32_t max_frame_size = 16384;
        std::uint32_t max_header_list_size = 0xffffffff;
    };

    struct LocalSettings
    {
        std::uint32_t header_table_size = 4096;
        bool enable_push = false;
        std::uint32_t max_concurrent_streams = 128;
        std::uint32_t initial_window_size = 65535;
        std::uint32_t max_frame_size = 16384;
        std::uint32_t max_header_list_size = 65536;
    };

    class Session
    {
    public:
        enum class StreamState : std::uint8_t
        {
            idle,
            open,
            half_closed_remote,
            half_closed_local,
            reserved_local,
            reserved_remote,
            closed
        };

        using HeadersBridge = std::function<void(std::uint32_t stream_id, std::string_view raw_headers, bool end_stream)>;
        using DataBridge = std::function<void(std::uint32_t stream_id, const std::vector<std::uint8_t> &data, bool end_stream)>;
        using GoawayBridge = std::function<void(ErrorCode error, std::uint32_t last_stream_id)>;

        explicit Session(const std::shared_ptr<net::Connection> &conn);

        bool on_preface_received();
        bool on_bytes(::yuan::buffer::ByteBuffer &input);
        void send_simple_response(std::uint32_t stream_id,
                                  std::uint16_t status,
                                  std::string_view content_type,
                                  std::string_view body);
        void send_headers(std::uint32_t stream_id,
                          const std::vector<std::pair<std::string_view, std::string_view>> &headers,
                          bool end_stream);
        void send_data(std::uint32_t stream_id, const std::uint8_t *data, std::size_t len, bool end_stream);
        void send_data(std::uint32_t stream_id, ::yuan::buffer::ByteBuffer body, bool end_stream);
        void send_data(std::uint32_t stream_id, std::string_view body, bool end_stream);
        void send_rst_stream(std::uint32_t stream_id, ErrorCode error);
        void send_goaway(ErrorCode error);
        void close_gracefully();

        std::uint64_t frames_received() const noexcept
        {
            return frames_received_;
        }

        void set_headers_bridge(HeadersBridge bridge)
        {
            headers_bridge_ = std::move(bridge);
        }

        void set_data_bridge(DataBridge bridge)
        {
            data_bridge_ = std::move(bridge);
        }

        void set_goaway_bridge(GoawayBridge bridge)
        {
            goaway_bridge_ = std::move(bridge);
        }

        const PeerSettings &peer_settings() const noexcept
        {
            return peer_settings_;
        }

        const LocalSettings &local_settings() const noexcept
        {
            return local_settings_;
        }

        std::uint32_t max_frame_size() const noexcept
        {
            return peer_settings_.max_frame_size;
        }

        std::uint32_t connection_send_window() const noexcept
        {
            return connection_send_window_;
        }

        std::uint32_t stream_send_window(std::uint32_t stream_id) const
        {
            auto it = streams_.find(stream_id);
            return it != streams_.end() ? it->second.send_window : 0;
        }

        StreamState stream_state(std::uint32_t stream_id) const
        {
            auto it = streams_.find(stream_id);
            return it != streams_.end() ? it->second.state : StreamState::closed;
        }

        bool stream_exists(std::uint32_t stream_id) const
        {
            return streams_.count(stream_id) > 0;
        }

        bool awaiting_settings_ack() const noexcept
        {
            return awaiting_settings_ack_;
        }

        std::size_t open_stream_count() const noexcept
        {
            std::size_t n = 0;
            for (const auto &p : streams_) {
                if (p.second.state == StreamState::open ||
                    p.second.state == StreamState::half_closed_local ||
                    p.second.state == StreamState::half_closed_remote) {
                    ++n;
                }
            }
            return n;
        }

        static constexpr std::uint32_t kMaxContinuationFrames = 16;
        static constexpr std::uint32_t kMaxHeaderBlockSize = 262144;

    private:
        struct PendingWrite
        {
            std::vector<std::uint8_t> data;
            std::size_t offset = 0;
            bool end_stream = false;
        };

        struct StreamEntry
        {
            StreamState state = StreamState::idle;
            std::uint64_t received_bytes = 0;
            std::vector<std::uint8_t> header_block;
            bool headers_end_stream = false;
            bool waiting_continuation = false;
            std::uint32_t send_window = 65535;
            std::uint32_t recv_window = 65535;
            PendingWrite pending_write;
            bool has_pending_write = false;
            std::uint32_t parent_stream_id = 0;
            std::uint8_t weight = 16;
            bool exclusive = false;
            std::uint32_t continuation_count = 0;
            bool received_data = false;
        };

        StreamEntry *get_or_create_stream(std::uint32_t stream_id);
        static std::string status_to_reason(std::uint16_t status);
        bool handle_frame(const Frame &frame);
        void send_frame(const Frame &frame);
        void send_server_settings();
        void apply_peer_settings(const Frame &settings_frame);
        void send_window_update(std::uint32_t stream_id, std::uint32_t increment);
        void check_and_send_window_update(std::uint32_t stream_id, std::size_t bytes_consumed);
        bool validate_stream_state(std::uint32_t stream_id, FrameType frame_type);
        void flush_pending_writes();

    private:
        std::shared_ptr<net::Connection> conn_;
        std::uint64_t frames_received_ = 0;
        bool seen_non_settings_frame_ = false;
        bool server_settings_sent_ = false;
        bool settings_ack_received_ = false;
        bool awaiting_settings_ack_ = false;
        std::unordered_map<std::uint32_t, StreamEntry> streams_;
        std::deque<std::uint32_t> pending_write_queue_;
        HpackDecoder hpack_decoder_;
        HpackEncoder hpack_encoder_;
        HeadersBridge headers_bridge_;
        DataBridge data_bridge_;
        GoawayBridge goaway_bridge_;
        PeerSettings peer_settings_;
        LocalSettings local_settings_;
        std::uint32_t connection_recv_window_ = 65535;
        std::uint32_t connection_send_window_ = 65535;
        std::uint32_t last_stream_id_ = 0;
    };
}

#endif
