#include "mqtt_session.h"
#include <algorithm>
#include <atomic>

namespace yuan::net::mqtt
{
    static std::atomic<uint64_t> global_session_id{ 1 };

    MqttSession::MqttSession(TcpConnection * conn)
        : session_id_(global_session_id.fetch_add(1)), last_activity_(std::chrono::steady_clock::now()), conn_(conn)
    {
    }

    uint16_t MqttSession::next_packet_id()
    {
        uint16_t id = next_packet_id_++;
        if (next_packet_id_ == 0)
            next_packet_id_ = 1;
        return id;
    }

    bool MqttSession::add_inflight_packet_id(uint16_t pid)
    {
        auto result = inflight_packet_ids_.insert(pid);
        return result.second;
    }

    bool MqttSession::has_inflight_packet_id(uint16_t pid) const
    {
        return inflight_packet_ids_.count(pid) > 0;
    }

    void MqttSession::remove_inflight_packet_id(uint16_t pid)
    {
        inflight_packet_ids_.erase(pid);
    }

    void MqttSession::add_outgoing_packet_id(uint16_t pid)
    {
        outgoing_packet_ids_.insert(pid);
    }

    bool MqttSession::has_outgoing_packet_id(uint16_t pid) const
    {
        return outgoing_packet_ids_.count(pid) > 0;
    }

    void MqttSession::remove_outgoing_packet_id(uint16_t pid)
    {
        outgoing_packet_ids_.erase(pid);
    }

    QoS MqttSession::subscription_qos(const std::string & filter) const
    {
        auto it = subscriptions_.find(filter);
        if (it != subscriptions_.end())
            return it->second;
        return QoS::AT_MOST_ONCE;
    }

    const MqttWillMessage *MqttSession::will_message() const
    {
        if (will_message_.has_value())
            return &(*will_message_);
        return nullptr;
    }

    void MqttSession::set_will_message(MqttWillMessage will)
    {
        will_message_ = std::move(will);
    }

    void MqttSession::clear_will_message()
    {
        will_message_.reset();
    }

    void MqttSession::set_topic_alias(uint16_t alias, const std::string & topic)
    {
        topic_aliases_[alias] = topic;
    }

    std::optional<std::string> MqttSession::resolve_topic_alias(uint16_t alias) const
    {
        auto it = topic_aliases_.find(alias);
        if (it != topic_aliases_.end())
            return it->second;
        return std::nullopt;
    }

    MqttSession &MqttSessionManager::create_session(TcpConnection * conn)
    {
        auto session = std::make_unique<MqttSession>(conn);
        auto &ref = *session;
        sessions_[ref.session_id()] = std::move(session);
        return ref;
    }

    void MqttSessionManager::remove_session(uint64_t sid)
    {
        auto it = sessions_.find(sid);
        if (it != sessions_.end()) {
            const auto &client_id = it->second->client_id();
            if (!client_id.empty())
                client_id_index_.erase(client_id);
            sessions_.erase(it);
        }
    }

    MqttSession *MqttSessionManager::find_by_client_id(const std::string & client_id)
    {
        auto idx_it = client_id_index_.find(client_id);
        if (idx_it == client_id_index_.end())
            return nullptr;
        auto sess_it = sessions_.find(idx_it->second);
        if (sess_it != sessions_.end())
            return sess_it->second.get();
        return nullptr;
    }

    MqttSession *MqttSessionManager::find_by_connection(TcpConnection * conn)
    {
        for (auto &pair : sessions_) {
            if (pair.second->connection() == conn)
                return pair.second.get();
        }
        return nullptr;
    }

    std::vector<MqttSession *> MqttSessionManager::all_sessions()
    {
        std::vector<MqttSession *> result;
        result.reserve(sessions_.size());
        for (auto &pair : sessions_)
            result.push_back(pair.second.get());
        return result;
    }

    void MqttSessionManager::cleanup_expired()
    {
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto &session = it->second;
            if (session->state() == MqttSessionState::disconnected &&
                session->session_expiry_interval() > 0) {
                auto expiry = session->last_activity() +
                              std::chrono::seconds(session->session_expiry_interval());
                if (now >= expiry) {
                    const auto &client_id = session->client_id();
                    if (!client_id.empty())
                        client_id_index_.erase(client_id);
                    it = sessions_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
}
