#ifndef __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#define __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#include "buffer/linked_buffer.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "timer/timer_manager.h"
#include "websocket_protocol.h"

#include <memory>
#include <string>

namespace net::websocket 
{
    class WebSocketHandler;
    class ProtoChunk;

    class WebSocketConnection
    {
        friend class WebSocketPacketParser;
    public:
        enum class WorkMode
        {
            client_,
            server_
        };

        enum class State
        {
            connecting_,
            connected,
            closing,
            closed
        };

        enum class PacketType : uint8_t
        {
            text_ = (uint8_t)OpCodeType::type_text_frame,
            binary_ = (uint8_t)OpCodeType::type_binary_frame,
            close_ = (uint8_t)OpCodeType::type_close_frame,
            ping_ = (uint8_t)OpCodeType::type_ping_frame,
            pong_ = (uint8_t)OpCodeType::type_pong_frame,
        };

    public:
        WebSocketConnection();
        ~WebSocketConnection();

    public:
        void on_created(Connection *conn);
        
        bool send(Buffer *buf, PacketType pktType = WebSocketConnection::PacketType::text_);

        void close();

        void set_handler(WebSocketHandler *handler);

        net::Connection * get_native_connection();

        const std::string & get_url() const;

        void set_url(const std::string &url);

        State get_state() const;

        bool connected() const
        {
            return get_state() == State::connected;
        }

        void try_set_heartbeat_timer(timer::TimerManager *timerManager);

    private:
        void free_self();

        std::vector<Buffer *> * get_output_buffers();

        std::vector<ProtoChunk> * get_input_chunks();
        
    private:
        class ConnData;
        std::unique_ptr<ConnData> data_;
    };
}

#endif