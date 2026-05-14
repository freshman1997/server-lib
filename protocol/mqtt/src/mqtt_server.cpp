#include "mqtt_server.h"
#include "mqtt_codec.h"
#include "mqtt_packet.h"
#include "net/channel/channel.h"
#include "net/connection/tcp_connection.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace yuan::net::mqtt
{
    MqttServer::MqttServer()
        : config_(), dispatcher_(config_, session_mgr_, topic_tree_, retained_store_, handler_)
    {
    }

    MqttServer::MqttServer(const MqttServerConfig & config)
        : config_(config), dispatcher_(config_, session_mgr_, topic_tree_, retained_store_, handler_)
    {
    }

    MqttServer::~MqttServer()
    {
        stop();
        listener_.close();
    }

    bool MqttServer::init(int port)
    {
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return init(port, *owned_runtime_);
    }

    bool MqttServer::init(int port, NetworkRuntime & runtime)
    {
        return listener_.bind(port, runtime);
    }

    void MqttServer::serve()
    {
        listener_.set_connection_handler(
            [this](AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        auto task = listener_.run_async();
        task.resume();
        task.detach();

        if (owned_runtime_) {
            owned_runtime_->run();
        }
    }

    void MqttServer::stop()
    {
        if (owned_runtime_) {
            auto *runtime = owned_runtime_.get();
            runtime->dispatch([this, runtime]() {
                listener_.close();

                auto sessions = session_mgr_.all_sessions();
                std::vector<std::shared_ptr<TcpConnection>> connections;
                connections.reserve(sessions.size());
                for (auto *session : sessions) {
                    if (!session) {
                        continue;
                    }
                    if (auto conn = session->connection()) {
                        connections.push_back(std::move(conn));
                    }
                }

                for (auto &conn : connections) {
                    if (conn) {
                        conn->close();
                    }
                }
                runtime->stop();
            });
        }
    }

    void MqttServer::set_handler(MqttHandler * handler)
    {
        handler_ = handler;
        dispatcher_.set_handler(handler);
    }

    coroutine::Task<void> MqttServer::handle_connection(AsyncConnectionContext ctx)
    {
        auto conn = ctx.connection();
        auto tcp_conn = std::dynamic_pointer_cast<TcpConnection>(conn);
        if (!tcp_conn) {
            ctx.close();
            co_return;
        }

        auto session_owner = session_mgr_.create_session_owner(tcp_conn);
        auto &session = *session_owner;
        session.set_state(MqttSessionState::connecting);
        LOG_DEBUG("[mqtt] session {} start fd={}", session.session_id(), tcp_conn->stream_channel() ? tcp_conn->stream_channel()->get_fd() : -1);

        bool cleaned_up = false;
        bool ever_connected = false;
        auto cleanup_session = [&]() {
            if (cleaned_up) {
                return;
            }
            cleaned_up = true;
            if (ever_connected) {
                dispatcher_.on_session_closed(session);
            } else {
                session.set_state(MqttSessionState::disconnected);
            }
            session_mgr_.remove_session(session.session_id());
        };

        ByteBuffer recv_buf;
        bool first_packet = true;

        while (ctx.is_connected()) {
            uint32_t timeout_ms = config_.idle_timeout_ms;
            auto read_result = co_await ctx.read_async(timeout_ms);
            if (read_result.status != coroutine::IoStatus::success) {
                LOG_DEBUG("[mqtt] session {} read end status={}", session.session_id(), static_cast<int>(read_result.status));
                break;
            }

            recv_buf.append(read_result.data);
            session.update_last_activity();

            while (recv_buf.readable_bytes() > 0) {
                auto decoded = MqttCodec::try_decode(
                    reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()),
                    recv_buf.readable_bytes());

                if (!decoded) {
                    if (recv_buf.readable_bytes() > config_.max_packet_size) {
                        if (session.protocol_level() == ProtocolLevel::V5_0) {
                            auto disc = MqttCodec::encode_disconnect(
                                static_cast<uint8_t>(DisconnectReason::PACKET_TOO_LARGE),
                                session.protocol_level(), {});
                            if (disc.readable_bytes() > 0) {
                                co_await ctx.write_async(std::move(disc));
                            }
                        }
                        ctx.close();
                        cleanup_session();
                        co_return;
                    }
                    break;
                }

                auto[
                    type,
                    pkt_len
                ] = *decoded;
                LOG_DEBUG("[mqtt] session {} packet type={} len={} first={} in_readable={}",
                          session.session_id(),
                          static_cast<int>(type),
                          pkt_len,
                          first_packet ? 1 : 0,
                          read_result.data.readable_bytes());

                if (pkt_len > config_.max_packet_size) {
                    if (session.protocol_level() == ProtocolLevel::V5_0) {
                        auto disc = MqttCodec::encode_disconnect(
                            static_cast<uint8_t>(DisconnectReason::PACKET_TOO_LARGE),
                            session.protocol_level(), {});
                        if (disc.readable_bytes() > 0) {
                            co_await ctx.write_async(std::move(disc));
                        }
                    }
                    ctx.close();
                    cleanup_session();
                    co_return;
                }

                if (first_packet) {
                    if (type != PacketType::CONNECT) {
                        ctx.close();
                        cleanup_session();
                        co_return;
                    }
                    first_packet = false;
                }

                if (type != PacketType::CONNECT &&
                    session.state() != MqttSessionState::connected) {
                    ctx.close();
                    cleanup_session();
                    co_return;
                }

                auto response = dispatcher_.dispatch(session,
                                                     reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()), pkt_len);
                LOG_DEBUG("[mqtt] session {} response bytes={} state={}", session.session_id(), response.readable_bytes(), static_cast<int>(session.state()));
                if (session.state() == MqttSessionState::connected) {
                    ever_connected = true;
                }

                if (first_packet) {
                    first_packet = false;
                }

                recv_buf.consume(pkt_len);

                if (response.readable_bytes() > 0) {
                    int write_fd = tcp_conn->stream_channel() ? tcp_conn->stream_channel()->get_fd() : -1;
                    LOG_DEBUG("[mqtt] session {} write_async fd={} bytes={}", session.session_id(), write_fd, response.readable_bytes());
                    auto wr = co_await ctx.write_async(std::move(response));
                    LOG_DEBUG("[mqtt] session {} write status={} fd={}", session.session_id(), static_cast<int>(wr.status), write_fd);
                }

                if (session.state() == MqttSessionState::disconnected ||
                    session.state() == MqttSessionState::disconnecting) {
                    ctx.close();
                    cleanup_session();
                    co_return;
                }
            }

            if (recv_buf.readable_bytes() == 0) {
                recv_buf.clear();
            } else {
                recv_buf.compact();
            }
        }

        cleanup_session();
        LOG_DEBUG("[mqtt] session {} cleanup done", session.session_id());
        ctx.close();
        co_return;
    }

    void MqttServer::publish(const std::string & topic, const std::vector<uint8_t> & payload,
                             QoS qos, bool retain)
    {
        if (retain) {
            MqttRetainedMessage msg;
            msg.topic = topic;
            msg.payload = payload;
            msg.qos = qos;
            msg.stored_time = std::chrono::steady_clock::now();
            retained_store_.store(msg);
        }

        auto subscribers = topic_tree_.match(topic);
        auto sessions = session_mgr_.all_sessions();

        for (const auto &sub : subscribers) {
            MqttSession *target = nullptr;
            for (auto *s : sessions) {
                if (s->session_id() == sub.session_id) {
                    target = s;
                    break;
                }
            }

            if (!target || target->state() != MqttSessionState::connected) {
                continue;
            }

            QoS effective_qos = std::min(qos, sub.qos);
            auto pkt = dispatcher_.build_publish_for_session(
                *target, topic, payload, effective_qos, false);

            if (pkt.readable_bytes() > 0 && target->connection()) {
                target->connection()->write_and_flush(pkt);
            }
        }
    }

    size_t MqttServer::connected_clients() const
    {
        return session_mgr_.all_sessions().size();
    }

    bool MqttServer::save_retained_store(const std::string & path) const
    {
        return retained_store_.save_to_file(path);
    }

    bool MqttServer::load_retained_store(const std::string & path)
    {
        return retained_store_.load_from_file(path);
    }

    bool MqttServer::save_session_store(const std::string & path) const
    {
        return session_mgr_.save_to_file(path);
    }

    bool MqttServer::load_session_store(const std::string & path)
    {
        return session_mgr_.load_from_file(path);
    }
}
