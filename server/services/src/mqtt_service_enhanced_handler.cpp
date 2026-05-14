#include "mqtt_service_enhanced_handler.h"

#include "mqtt_topic_tree.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace yuan::server
{
    namespace
    {
        std::vector<std::string> split_topic(const std::string & topic)
        {
            std::vector<std::string> levels;
            size_t start = 0;
            size_t pos = topic.find('/');
            while (pos != std::string::npos) {
                levels.push_back(topic.substr(start, pos - start));
                start = pos + 1;
                pos = topic.find('/', start);
            }
            levels.push_back(topic.substr(start));
            return levels;
        }
    }

    void MqttEnhancedHandler::set_allow_anonymous(bool allow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        allow_anonymous_ = allow;
    }

    bool MqttEnhancedHandler::allow_anonymous() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return allow_anonymous_;
    }

    void MqttEnhancedHandler::set_default_publish_allow(bool allow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        default_publish_allow_ = allow;
    }

    void MqttEnhancedHandler::set_default_subscribe_allow(bool allow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        default_subscribe_allow_ = allow;
    }

    void MqttEnhancedHandler::upsert_user(const std::string & username, const std::string & password)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        users_[username] = password;
    }

    void MqttEnhancedHandler::remove_user(const std::string & username)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        users_.erase(username);
    }

    void MqttEnhancedHandler::clear_users()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        users_.clear();
    }

    void MqttEnhancedHandler::add_publish_acl(const std::string & topic_filter, bool allow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publish_acl_.push_back({ topic_filter, allow });
    }

    void MqttEnhancedHandler::add_subscribe_acl(const std::string & topic_filter, bool allow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribe_acl_.push_back({ topic_filter, allow });
    }

    void MqttEnhancedHandler::clear_publish_acl()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publish_acl_.clear();
    }

    void MqttEnhancedHandler::clear_subscribe_acl()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscribe_acl_.clear();
    }

    const MqttEnhancedHandler::Metrics & MqttEnhancedHandler::metrics() const
    {
        return metrics_;
    }

    bool MqttEnhancedHandler::on_connect(yuan::net::mqtt::MqttSession * session,
                                         const std::string &,
                                         const std::string & username,
                                         const std::string & password)
    {
        metrics_.connect_attempts.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(mutex_);

        bool ok = false;
        if (username.empty()) {
            ok = allow_anonymous_;
        } else {
            auto it = users_.find(username);
            ok = (it != users_.end() && it->second == password);
        }

        if (ok) {
            metrics_.connect_accepted.fetch_add(1, std::memory_order_relaxed);
            if (session) {
                session_username_[session->session_id()] = username;
            }
        } else {
            metrics_.connect_rejected.fetch_add(1, std::memory_order_relaxed);
        }

        return ok;
    }

    bool MqttEnhancedHandler::on_auth(yuan::net::mqtt::MqttSession *,
                                      const std::string & method,
                                      const std::vector<uint8_t> &)
    {
        metrics_.auth_attempts.fetch_add(1, std::memory_order_relaxed);
        if (method.empty()) {
            metrics_.auth_rejected.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        metrics_.auth_accepted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void MqttEnhancedHandler::on_connected(yuan::net::mqtt::MqttSession *)
    {
        metrics_.connected_sessions.fetch_add(1, std::memory_order_relaxed);
    }

    void MqttEnhancedHandler::on_disconnected(yuan::net::mqtt::MqttSession * session, uint8_t)
    {
        const auto prev = metrics_.connected_sessions.fetch_sub(1, std::memory_order_relaxed);
        if (prev == 0) {
            metrics_.connected_sessions.store(0, std::memory_order_relaxed);
        }

        if (!session)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        session_username_.erase(session->session_id());
    }

    bool MqttEnhancedHandler::on_publish(yuan::net::mqtt::MqttSession *,
                                         const std::string & topic,
                                         const std::vector<uint8_t> &,
                                         yuan::net::mqtt::QoS,
                                         bool)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool allow = eval_acl(publish_acl_, topic, default_publish_allow_);
        if (allow)
            metrics_.publish_allowed.fetch_add(1, std::memory_order_relaxed);
        else
            metrics_.publish_denied.fetch_add(1, std::memory_order_relaxed);
        return allow;
    }

    bool MqttEnhancedHandler::on_subscribe(yuan::net::mqtt::MqttSession *,
                                           const std::string & topic_filter,
                                           yuan::net::mqtt::QoS)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool allow = eval_acl(subscribe_acl_, topic_filter, default_subscribe_allow_);
        if (allow)
            metrics_.subscribe_allowed.fetch_add(1, std::memory_order_relaxed);
        else
            metrics_.subscribe_denied.fetch_add(1, std::memory_order_relaxed);
        return allow;
    }

    bool MqttEnhancedHandler::topic_matches_filter(const std::string & topic, const std::string & filter)
    {
        if (!yuan::net::mqtt::MqttTopicTree::validate_topic_filter(filter))
            return false;
        return [&]() {
            const auto topic_levels = split_topic(topic);
            const auto filter_levels = split_topic(filter);
            size_t ti = 0;
            size_t fi = 0;
            while (fi < filter_levels.size()) {
                if (filter_levels[fi] == "#")
                    return true;
                if (ti >= topic_levels.size())
                    return false;
                if (filter_levels[fi] == "+") {
                    ++fi;
                    ++ti;
                    continue;
                }
                if (filter_levels[fi] != topic_levels[ti])
                    return false;
                ++fi;
                ++ti;
            }
            return ti == topic_levels.size();
        }();
    }

    bool MqttEnhancedHandler::eval_acl(const std::vector<AclRule> & rules, const std::string & topic, bool default_allow) const
    {
        for (const auto &rule : rules) {
            if (topic_matches_filter(topic, rule.topic_filter)) {
                return rule.allow;
            }
        }
        return default_allow;
    }

    bool MqttEnhancedHandler::save_policy_file(const std::string & path) const
    {
        if (path.empty())
            return false;

        nlohmann::json root;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            root["allow_anonymous"] = allow_anonymous_;
            root["default_publish_allow"] = default_publish_allow_;
            root["default_subscribe_allow"] = default_subscribe_allow_;

            nlohmann::json users = nlohmann::json::array();
            for (const auto &entry : users_) {
                users.push_back({ { "username", entry.first }, { "password", entry.second } });
            }
            root["users"] = std::move(users);

            nlohmann::json pub_acl = nlohmann::json::array();
            for (const auto &rule : publish_acl_) {
                pub_acl.push_back({ { "topic_filter", rule.topic_filter }, { "allow", rule.allow } });
            }
            root["publish_acl"] = std::move(pub_acl);

            nlohmann::json sub_acl = nlohmann::json::array();
            for (const auto &rule : subscribe_acl_) {
                sub_acl.push_back({ { "topic_filter", rule.topic_filter }, { "allow", rule.allow } });
            }
            root["subscribe_acl"] = std::move(sub_acl);
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << root.dump(2);
        return out.good();
    }

    bool MqttEnhancedHandler::load_policy_file(const std::string & path)
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

        if (!root.is_object())
            return false;

        std::lock_guard<std::mutex> lock(mutex_);

        if (root.contains("allow_anonymous") && root["allow_anonymous"].is_boolean())
            allow_anonymous_ = root["allow_anonymous"].get<bool>();
        if (root.contains("default_publish_allow") && root["default_publish_allow"].is_boolean())
            default_publish_allow_ = root["default_publish_allow"].get<bool>();
        if (root.contains("default_subscribe_allow") && root["default_subscribe_allow"].is_boolean())
            default_subscribe_allow_ = root["default_subscribe_allow"].get<bool>();

        users_.clear();
        if (root.contains("users") && root["users"].is_array()) {
            for (const auto &u : root["users"]) {
                if (!u.is_object())
                    continue;
                if (!u.contains("username") || !u.contains("password"))
                    continue;
                if (!u["username"].is_string() || !u["password"].is_string())
                    continue;
                users_[u["username"].get<std::string>()] = u["password"].get<std::string>();
            }
        }

        publish_acl_.clear();
        if (root.contains("publish_acl") && root["publish_acl"].is_array()) {
            for (const auto &r : root["publish_acl"]) {
                if (!r.is_object())
                    continue;
                if (!r.contains("topic_filter") || !r.contains("allow"))
                    continue;
                if (!r["topic_filter"].is_string() || !r["allow"].is_boolean())
                    continue;
                publish_acl_.push_back({ r["topic_filter"].get<std::string>(), r["allow"].get<bool>() });
            }
        }

        subscribe_acl_.clear();
        if (root.contains("subscribe_acl") && root["subscribe_acl"].is_array()) {
            for (const auto &r : root["subscribe_acl"]) {
                if (!r.is_object())
                    continue;
                if (!r.contains("topic_filter") || !r.contains("allow"))
                    continue;
                if (!r["topic_filter"].is_string() || !r["allow"].is_boolean())
                    continue;
                subscribe_acl_.push_back({ r["topic_filter"].get<std::string>(), r["allow"].get<bool>() });
            }
        }

        return true;
    }
}
