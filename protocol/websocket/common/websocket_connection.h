#ifndef __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#define __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#include "buffer/byte_buffer.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "websocket_protocol.h"
#include "close_code.h"
#include "handshake.h"
#include "websocket_packet_parser.h"

#include <functional>
#include <memory>
#include <cstddef>
#include <string>
#include <vector>

namespace yuan::net::websocket
{
    class WebSocketConfigManager;

    enum class FrameDispatchResult {
        ok_,
        close_,
        error_
    };

    class WebSocketConnection : public std::enable_shared_from_this<WebSocketConnection>
    {
        friend class WebSocketPacketParser;

    public:
        using WorkMode = ::yuan::net::websocket::WorkMode;

        enum class State {
            connecting_,
            connected_,
            closing_,
            closed_
        };

        enum class PacketType : uint8_t {
            text_ = (uint8_t)OpCodeType::type_text_frame,
            binary_ = (uint8_t)OpCodeType::type_binary_frame,
            close_ = (uint8_t)OpCodeType::type_close_frame,
            ping_ = (uint8_t)OpCodeType::type_ping_frame,
            pong_ = (uint8_t)OpCodeType::type_pong_frame,
        };

        using DataCallback = std::function<void(WebSocketConnection *, const ::yuan::buffer::ByteBuffer &)>;

    public:
        explicit WebSocketConnection(WorkMode mode = WorkMode::server_);
        ~WebSocketConnection();

        WebSocketConnection(const WebSocketConnection &) = delete;
        WebSocketConnection &operator=(const WebSocketConnection &) = delete;

        void bind_connection(Connection *conn);
        void bind_connection(const std::shared_ptr<Connection> &conn);
        std::shared_ptr<Connection> connection() const
        {
            return conn_owner_.lock();
        }

        void set_config(WebSocketConfigManager *config);

        bool send(const ::yuan::buffer::ByteBuffer &buf, PacketType pktType = PacketType::text_);
        bool send(const char *data, size_t len, PacketType pktType = PacketType::text_);

        void close(WebSocketCloseCode code = WebSocketCloseCode::normal_close_);

        void shutdown();

        std::shared_ptr<Connection> get_native_connection();

        const std::string &get_url() const;
        void set_url(const std::string &url);

        State get_state() const;
        bool connected() const
        {
            return get_state() == State::connected_;
        }

        void set_state(State state);

        void mark_connected();

        void use_mask(bool on);

        void try_set_heartbeat_timer(NetworkRuntime *runtime);

        FrameDispatchResult dispatch_frames(Connection *conn);
        FrameDispatchResult dispatch_frames(const std::shared_ptr<Connection> &conn);

    public:
        WebSocketHandshaker &handshaker();
        WebSocketPacketParser &pkt_parser();
        const std::vector<ProtoChunk> &input_chunks() const;
        std::vector<ProtoChunk> &input_chunks();
        void clear_input_chunks();

        bool pack_frame(const ::yuan::buffer::ByteBuffer &data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> &output);

        bool pack_control_frame(const ::yuan::buffer::ByteBuffer &data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> &output);

        void set_outbound_queue_limits(std::size_t max_messages, std::size_t max_bytes);

        std::size_t outbound_queue_size() const;

        std::size_t outbound_queue_bytes() const;

        bool close_sent() const;

        bool close_received() const;

        bool force_closed() const;

        bool close_handshake_expired(uint32_t now_ms = 0) const;

        uint32_t last_read_time() const;

        uint32_t last_write_time() const;

        uint32_t last_ping_time() const;

        uint32_t last_pong_time() const;

        uint32_t max_message_size() const;

        uint32_t max_fragmented_message_size() const;

        void send_ping_frame_to(Connection *conn);
        void send_ping_frame_to(const std::shared_ptr<Connection> &conn);
        void send_pong_frame_to(Connection *conn, const ::yuan::buffer::ByteBuffer &payload = {});
        void send_pong_frame_to(const std::shared_ptr<Connection> &conn, const ::yuan::buffer::ByteBuffer &payload = {});
        void send_close_frame_to(Connection *conn, uint16_t code);
        void send_close_frame_to(const std::shared_ptr<Connection> &conn, uint16_t code);

        DataCallback on_data;
        std::function<void(WebSocketConnection *)> on_connected_cb;
        std::function<void(WebSocketConnection *)> on_close_cb;

    private:
        std::vector< ::yuan::buffer::ByteBuffer> *get_output_buffers();
        std::vector<ProtoChunk> *get_input_chunks();
        bool enqueue_output(std::vector< ::yuan::buffer::ByteBuffer> &&output);
        bool flush_output_queue();
        void mark_close_sent();
        void mark_close_received();
        void mark_read_activity();
        void mark_write_activity();
        void mark_ping_sent();
        void mark_pong_received();
        void check_pong_deadline();
        void schedule_close_deadline_timer();
        void cancel_close_deadline_timer();
        void force_close();
        std::weak_ptr<WebSocketConnection> weak_self() noexcept;

    private:
        WorkMode mode_;
        State state_;
        std::string url_;
        std::weak_ptr<Connection> conn_owner_;
        Connection *conn_;
        NetworkRuntime *runtime_ = nullptr;
        timer::TimerHandle heartbeat_timer_;
        timer::TimerHandle close_deadline_timer_;
        WebSocketHandshaker handshaker_;
        WebSocketPacketParser pkt_parser_;
        WebSocketConfigManager *config_;
        std::vector<ProtoChunk> input_chunks_;
        std::vector< ::yuan::buffer::ByteBuffer> output_chunks_;
        std::vector< ::yuan::buffer::ByteBuffer> outbound_queue_;
        std::size_t outbound_queue_bytes_ = 0;
        std::size_t outbound_queue_max_messages_ = 0;
        std::size_t outbound_queue_max_bytes_ = 0;
        uint32_t last_read_time_ = 0;
        uint32_t last_write_time_ = 0;
        uint32_t last_ping_time_ = 0;
        uint32_t last_pong_time_ = 0;
        uint32_t pong_deadline_ms_ = 0;
        uint32_t close_handshake_timeout_ms_ = DEFAULT_CLOSE_HANDSHAKE_TIMEOUT_MS;
        uint32_t max_message_size_ = PACKET_MAX_BYTE;
        uint32_t max_fragmented_message_size_ = PACKET_MAX_BYTE;
        uint32_t close_deadline_ms_ = 0;
        bool close_sent_ = false;
        bool close_received_ = false;
        bool force_closed_ = false;
    };
}
#endif
