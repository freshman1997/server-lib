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

    class WebSocketConnection
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

        void set_config(WebSocketConfigManager *config);

        bool send(const ::yuan::buffer::ByteBuffer &buf, PacketType pktType = PacketType::text_);
        bool send(const char *data, size_t len, PacketType pktType = PacketType::text_);

        void close(WebSocketCloseCode code = WebSocketCloseCode::normal_close_);

        Connection *get_native_connection();

        const std::string &get_url() const;
        void set_url(const std::string &url);

        State get_state() const;
        bool connected() const
        {
            return get_state() == State::connected_;
        }

        void set_state(State state);

        void use_mask(bool on);

        void try_set_heartbeat_timer(NetworkRuntime *runtime);

        FrameDispatchResult dispatch_frames(Connection *conn);

    public:
        WebSocketHandshaker &handshaker();
        WebSocketPacketParser &pkt_parser();
        const std::vector<ProtoChunk> &input_chunks() const;
        std::vector<ProtoChunk> &input_chunks();
        void clear_input_chunks();

        bool pack_frame(const ::yuan::buffer::ByteBuffer &data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> &output);

        bool pack_control_frame(const ::yuan::buffer::ByteBuffer &data, uint8_t type, std::vector< ::yuan::buffer::ByteBuffer> &output);

        void send_ping_frame_to(Connection *conn);
        void send_pong_frame_to(Connection *conn, const ::yuan::buffer::ByteBuffer &payload = {});
        void send_close_frame_to(Connection *conn, uint16_t code);

        DataCallback on_data;
        std::function<void(WebSocketConnection *)> on_connected_cb;
        std::function<void(WebSocketConnection *)> on_close_cb;

    private:
        std::vector< ::yuan::buffer::ByteBuffer> *get_output_buffers();
        std::vector<ProtoChunk> *get_input_chunks();

    private:
        WorkMode mode_;
        State state_;
        std::string url_;
        Connection *conn_;
        timer::Timer *heartbeat_timer_;
        WebSocketHandshaker handshaker_;
        WebSocketPacketParser pkt_parser_;
        WebSocketConfigManager *config_;
        std::vector<ProtoChunk> input_chunks_;
        std::vector< ::yuan::buffer::ByteBuffer> output_chunks_;
        uint32_t last_active_time_;
    };
}
#endif
