#include "mqtt_session.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

namespace yuan::net::mqtt
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

    }

    static std::atomic<uint64_t> global_session_id{ 1 };

    MqttSession::MqttSession(TcpConnection * conn)
        : session_id_(global_session_id.fetch_add(1)), last_activity_(std::chrono::steady_clock::now()), conn_(conn)
    {
    }

    MqttSession::MqttSession(const std::shared_ptr<TcpConnection> &conn)
        : session_id_(global_session_id.fetch_add(1)), last_activity_(std::chrono::steady_clock::now()), conn_owner_(conn), conn_(ptr_of(conn))
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
        auto session = std::make_shared<MqttSession>(conn);
        auto &ref = *session;
        sessions_[ref.session_id()] = std::move(session);
        return ref;
    }

    MqttSession &MqttSessionManager::create_session(const std::shared_ptr<TcpConnection> &conn)
    {
        auto session = create_session_owner(conn);
        return *session;
    }

    std::shared_ptr<MqttSession> MqttSessionManager::create_session_owner(const std::shared_ptr<TcpConnection> &conn)
    {
        auto session = std::make_shared<MqttSession>(conn);
        auto &ref = *session;
        sessions_[ref.session_id()] = session;
        return session;
    }

    void MqttSessionManager::bind_client_id(MqttSession & session, const std::string & client_id)
    {
        const auto &old_client_id = session.client_id();
        if (!old_client_id.empty() && old_client_id != client_id)
            client_id_index_.erase(old_client_id);

        session.set_client_id(client_id);
        if (!client_id.empty())
            client_id_index_[client_id] = session.session_id();
    }

    void MqttSessionManager::remove_session(uint64_t sid)
    {
        auto it = sessions_.find(sid);
        if (it != sessions_.end()) {
            const auto &client_id = it->second->client_id();
            if (!client_id.empty()) {
                auto idx_it = client_id_index_.find(client_id);
                if (idx_it != client_id_index_.end() && idx_it->second == sid) {
                    client_id_index_.erase(idx_it);
                }
            }
            sessions_.erase(it);
        }
    }

    MqttSession *MqttSessionManager::find_by_client_id(const std::string & client_id)
    {
        auto idx_it = client_id_index_.find(client_id);
        if (idx_it != client_id_index_.end()) {
            auto sess_it = sessions_.find(idx_it->second);
            if (sess_it != sessions_.end() && sess_it->second->client_id() == client_id)
                return ptr_of(sess_it->second);
        }

        for (auto &pair : sessions_) {
            if (pair.second->client_id() == client_id) {
                client_id_index_[client_id] = pair.first;
                return ptr_of(pair.second);
            }
        }

        return nullptr;
    }

    MqttSession *MqttSessionManager::find_by_connection(TcpConnection * conn)
    {
        for (auto &pair : sessions_) {
            if (auto current = pair.second->connection(); current && ptr_of(current) == conn)
                return ptr_of(pair.second);
        }
        return nullptr;
    }

    std::vector<MqttSession *> MqttSessionManager::all_sessions() const
    {
        std::vector<MqttSession *> result;
        result.reserve(sessions_.size());
        for (auto &pair : sessions_)
            result.push_back(ptr_of(pair.second));
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

    bool MqttSessionManager::save_to_file(const std::string & path) const
    {
        if (path.empty())
            return false;

        nlohmann::json root = nlohmann::json::array();
        for (const auto &pair : sessions_) {
            const auto &session = pair.second;
            if (!session)
                continue;

            nlohmann::json item;
            item["session_id"] = session->session_id();
            item["client_id"] = session->client_id();
            item["protocol_level"] = static_cast<uint8_t>(session->protocol_level());
            item["state"] = static_cast<int>(session->state());
            item["keep_alive"] = session->keep_alive();
            item["clean_start"] = session->clean_start();
            item["session_expiry_interval"] = session->session_expiry_interval();
            item["receive_maximum"] = session->receive_maximum();
            item["client_receive_maximum"] = session->client_receive_maximum();
            item["maximum_packet_size"] = session->maximum_packet_size();
            item["topic_alias_maximum"] = session->topic_alias_maximum();

            nlohmann::json subs = nlohmann::json::array();
            for (const auto &sub : session->subscriptions()) {
                subs.push_back({ { "topic_filter", sub.first }, { "qos", static_cast<uint8_t>(sub.second) } });
            }
            item["subscriptions"] = std::move(subs);

            if (const auto *will = session->will_message()) {
                nlohmann::json jwill;
                jwill["topic"] = will->topic;
                jwill["payload"] = will->payload;
                jwill["qos"] = static_cast<uint8_t>(will->qos);
                jwill["retain"] = will->retain;
                if (will->will_delay_interval.has_value())
                    jwill["will_delay_interval"] = *will->will_delay_interval;
                if (will->payload_format_indicator.has_value())
                    jwill["payload_format_indicator"] = *will->payload_format_indicator;
                if (will->message_expiry_interval.has_value())
                    jwill["message_expiry_interval"] = *will->message_expiry_interval;
                if (will->content_type.has_value())
                    jwill["content_type"] = *will->content_type;
                if (will->response_topic.has_value())
                    jwill["response_topic"] = *will->response_topic;
                if (will->correlation_data.has_value())
                    jwill["correlation_data"] = *will->correlation_data;

                nlohmann::json up = nlohmann::json::array();
                for (const auto &p : will->user_properties) {
                    up.push_back({ { "key", p.key }, { "value", p.value } });
                }
                jwill["user_properties"] = std::move(up);
                item["will_message"] = std::move(jwill);
            }

            root.push_back(std::move(item));
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << root.dump(2);
        return out.good();
    }

    bool MqttSessionManager::load_from_file(const std::string & path)
    {
        if (path.empty())
            return false;

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return false;

        nlohmann::json root;
        try {
            in >> root;
        } catch (...) {
            return false;
        }

        if (!root.is_array())
            return false;

        sessions_.clear();
        client_id_index_.clear();

        for (const auto &item : root) {
            if (!item.is_object())
                continue;

            auto session = std::make_shared<MqttSession>(static_cast<TcpConnection *>(nullptr));
            if (item.contains("client_id") && item["client_id"].is_string())
                session->set_client_id(item["client_id"].get<std::string>());

            if (item.contains("protocol_level") && item["protocol_level"].is_number_unsigned()) {
                const uint8_t lvl = item["protocol_level"].get<uint8_t>();
                if (lvl == static_cast<uint8_t>(ProtocolLevel::V5_0))
                    session->set_protocol_level(ProtocolLevel::V5_0);
                else
                    session->set_protocol_level(ProtocolLevel::V3_1_1);
            }

            if (item.contains("state") && item["state"].is_number_integer()) {
                const int st = item["state"].get<int>();
                if (st >= static_cast<int>(MqttSessionState::disconnected) &&
                    st <= static_cast<int>(MqttSessionState::disconnecting)) {
                    session->set_state(static_cast<MqttSessionState>(st));
                } else {
                    session->set_state(MqttSessionState::disconnected);
                }
            } else {
                session->set_state(MqttSessionState::disconnected);
            }

            if (item.contains("keep_alive") && item["keep_alive"].is_number_unsigned())
                session->set_keep_alive(item["keep_alive"].get<uint16_t>());
            if (item.contains("clean_start") && item["clean_start"].is_boolean())
                session->set_clean_start(item["clean_start"].get<bool>());
            if (item.contains("session_expiry_interval") && item["session_expiry_interval"].is_number_unsigned())
                session->set_session_expiry_interval(item["session_expiry_interval"].get<uint32_t>());
            if (item.contains("receive_maximum") && item["receive_maximum"].is_number_unsigned())
                session->set_receive_maximum(item["receive_maximum"].get<uint16_t>());
            if (item.contains("client_receive_maximum") && item["client_receive_maximum"].is_number_unsigned())
                session->set_client_receive_maximum(item["client_receive_maximum"].get<uint16_t>());
            if (item.contains("maximum_packet_size") && item["maximum_packet_size"].is_number_unsigned())
                session->set_maximum_packet_size(item["maximum_packet_size"].get<uint32_t>());
            if (item.contains("topic_alias_maximum") && item["topic_alias_maximum"].is_number_unsigned())
                session->set_topic_alias_maximum(item["topic_alias_maximum"].get<uint16_t>());

            if (item.contains("subscriptions") && item["subscriptions"].is_array()) {
                for (const auto &sub : item["subscriptions"]) {
                    if (!sub.is_object())
                        continue;
                    if (!sub.contains("topic_filter") || !sub.contains("qos"))
                        continue;
                    if (!sub["topic_filter"].is_string() || !sub["qos"].is_number_unsigned())
                        continue;
                    const auto filter = sub["topic_filter"].get<std::string>();
                    const auto qos_u8 = sub["qos"].get<uint8_t>();
                    const QoS qos = qos_u8 <= 2 ? static_cast<QoS>(qos_u8) : QoS::AT_MOST_ONCE;
                    session->add_subscription(filter, qos);
                }
            }

            if (item.contains("will_message") && item["will_message"].is_object()) {
                const auto &jwill = item["will_message"];
                MqttWillMessage will;
                if (jwill.contains("topic") && jwill["topic"].is_string())
                    will.topic = jwill["topic"].get<std::string>();
                if (jwill.contains("payload") && jwill["payload"].is_array())
                    will.payload = jwill["payload"].get<std::vector<uint8_t> >();
                if (jwill.contains("qos") && jwill["qos"].is_number_unsigned()) {
                    const auto qos_u8 = jwill["qos"].get<uint8_t>();
                    will.qos = qos_u8 <= 2 ? static_cast<QoS>(qos_u8) : QoS::AT_MOST_ONCE;
                }
                if (jwill.contains("retain") && jwill["retain"].is_boolean())
                    will.retain = jwill["retain"].get<bool>();
                if (jwill.contains("will_delay_interval") && jwill["will_delay_interval"].is_number_unsigned())
                    will.will_delay_interval = jwill["will_delay_interval"].get<uint32_t>();
                if (jwill.contains("payload_format_indicator") && jwill["payload_format_indicator"].is_number_unsigned())
                    will.payload_format_indicator = jwill["payload_format_indicator"].get<uint8_t>();
                if (jwill.contains("message_expiry_interval") && jwill["message_expiry_interval"].is_number_unsigned())
                    will.message_expiry_interval = jwill["message_expiry_interval"].get<uint32_t>();
                if (jwill.contains("content_type") && jwill["content_type"].is_string())
                    will.content_type = jwill["content_type"].get<std::string>();
                if (jwill.contains("response_topic") && jwill["response_topic"].is_string())
                    will.response_topic = jwill["response_topic"].get<std::string>();
                if (jwill.contains("correlation_data") && jwill["correlation_data"].is_array())
                    will.correlation_data = jwill["correlation_data"].get<std::vector<uint8_t> >();
                if (jwill.contains("user_properties") && jwill["user_properties"].is_array()) {
                    for (const auto &up : jwill["user_properties"]) {
                        if (!up.is_object())
                            continue;
                        if (!up.contains("key") || !up.contains("value"))
                            continue;
                        if (!up["key"].is_string() || !up["value"].is_string())
                            continue;
                        will.user_properties.push_back({ up["key"].get<std::string>(), up["value"].get<std::string>() });
                    }
                }
                if (!will.topic.empty()) {
                    session->set_will_message(std::move(will));
                }
            }

            const uint64_t sid = session->session_id();
            sessions_[sid] = session;
            if (!session->client_id().empty())
                client_id_index_[session->client_id()] = sid;
        }
        return true;
    }
}
