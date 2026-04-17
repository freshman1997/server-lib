#include "mqtt_server.h"
#include "mqtt_codec.h"
#include "mqtt_packet.h"
#include "net/connection/tcp_connection.h"
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
            owned_runtime_->stop();
        }
    }

    void MqttServer::set_handler(MqttHandler * handler)
    {
        handler_ = handler;
        dispatcher_.set_handler(handler);
    }

    coroutine::Task<void> MqttServer::handle_connection(AsyncConnectionContext ctx)
    {
        auto &session = session_mgr_.create_session(ctx.native_handle());
        session.set_state(MqttSessionState::connecting);

        ByteBuffer recv_buf;
        bool first_packet = true;

        while (ctx.is_connected()) {
            uint32_t timeout_ms = config_.idle_timeout_ms;
            auto read_result = co_await ctx.read_async(timeout_ms);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            recv_buf.append(read_result.data);
            session.update_last_activity();

            while (recv_buf.readable_bytes() > 0) {
                auto decoded = MqttCodec::try_decode(
                    reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()),
                    recv_buf.readable_bytes());

                if (!decoded) {
                    break;
                }

                auto[
                    type,
                    pkt_len
                ] = *decoded;

                if (first_packet) {
                    if (type != PacketType::CONNECT) {
                        ctx.close();
                        co_return;
                    }
                    first_packet = false;
                }

                if (type != PacketType::CONNECT &&
                    session.state() != MqttSessionState::connected) {
                    ctx.close();
                    co_return;
                }

                auto response = dispatcher_.dispatch(session,
                                                     reinterpret_cast<const uint8_t *>(recv_buf.read_ptr()), pkt_len);

                recv_buf.consume(pkt_len);

                if (response.readable_bytes() > 0) {
                    co_await ctx.write_async(std::move(response));
                }

                if (session.state() == MqttSessionState::disconnected ||
                    session.state() == MqttSessionState::disconnecting) {
                    ctx.close();
                    co_return;
                }
            }

            if (recv_buf.readable_bytes() == 0) {
                recv_buf.clear();
            } else {
                recv_buf.compact();
            }
        }

        dispatcher_.on_session_closed(session);
        session_mgr_.remove_session(session.session_id());
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
}
