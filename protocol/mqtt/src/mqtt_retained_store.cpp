#include "mqtt_retained_store.h"
#include "base/utils/string_util.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace yuan::net::mqtt
{
    static std::vector<std::string> split_topic(const std::string & topic)
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

    static bool topic_matches_filter(const std::vector<std::string> & topic_levels,
                                     const std::vector<std::string> & filter_levels)
    {
        size_t ti = 0;
        size_t fi = 0;

        while (fi < filter_levels.size()) {
            if (filter_levels[fi] == "#") {
                return true;
            }

            if (ti >= topic_levels.size()) {
                return false;
            }

            if (filter_levels[fi] == "+") {
                ++fi;
                ++ti;
                continue;
            }

            if (filter_levels[fi] != topic_levels[ti]) {
                return false;
            }

            ++fi;
            ++ti;
        }

        return ti == topic_levels.size();
    }

    void MqttRetainedStore::store(const MqttRetainedMessage & msg)
    {
        if (msg.payload.empty()) {
            messages_.erase(msg.topic);
            return;
        }

        MqttRetainedMessage stored = msg;
        stored.stored_time = std::chrono::steady_clock::now();
        messages_[msg.topic] = stored;
    }

    std::vector<MqttRetainedMessage> MqttRetainedStore::match(const std::string & topic_filter) const
    {
        std::vector<MqttRetainedMessage> result;

        if (topic_filter.empty()) {
            return result;
        }

        std::vector<std::string> filter_levels = split_topic(topic_filter);

        bool has_wildcard = topic_filter.find('+') != std::string::npos ||
                            topic_filter.find('#') != std::string::npos;

        for (const auto & [
                              topic,
                              msg
                          ] : messages_) {
            if (has_wildcard) {
                if (!topic.empty() && topic[0] == '$') {
                    bool filter_starts_with_wildcard = (topic_filter == "#" ||
                                                        topic_filter[0] == '+' ||
                                                        topic_filter[0] == '#');
                    if (filter_starts_with_wildcard) {
                        continue;
                    }
                }

                std::vector<std::string> topic_levels = split_topic(topic);
                if (topic_matches_filter(topic_levels, filter_levels)) {
                    result.push_back(msg);
                }
            } else {
                if (topic == topic_filter) {
                    result.push_back(msg);
                }
            }
        }

        return result;
    }

    void MqttRetainedStore::cleanup_expired()
    {
        auto now = std::chrono::steady_clock::now();

        for (auto it = messages_.begin(); it != messages_.end();) {
            const auto &msg = it->second;
            if (msg.message_expiry_interval.has_value()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - msg.stored_time);
                if (elapsed.count() >= static_cast<int64_t>(msg.message_expiry_interval.value())) {
                    it = messages_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    size_t MqttRetainedStore::size() const
    {
        return messages_.size();
    }

    bool MqttRetainedStore::save_to_file(const std::string & path) const
    {
        nlohmann::json root = nlohmann::json::array();
        for (const auto &entry : messages_) {
            const auto &msg = entry.second;
            nlohmann::json item;
            item["topic"] = msg.topic;
            item["payload_hex"] = yuan::base::util::to_hex(msg.payload);
            item["qos"] = static_cast<uint8_t>(msg.qos);

            if (msg.message_expiry_interval.has_value())
                item["message_expiry_interval"] = *msg.message_expiry_interval;
            if (msg.payload_format_indicator.has_value())
                item["payload_format_indicator"] = *msg.payload_format_indicator;
            if (msg.content_type.has_value())
                item["content_type"] = *msg.content_type;

            if (!msg.user_properties.empty()) {
                nlohmann::json ups = nlohmann::json::array();
                for (const auto &up : msg.user_properties) {
                    ups.push_back({ { "key", up.key }, { "value", up.value } });
                }
                item["user_properties"] = std::move(ups);
            }

            if (msg.message_expiry_interval.has_value()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - msg.stored_time);
                item["stored_elapsed_seconds"] = elapsed.count();
            }

            root.push_back(std::move(item));
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << root.dump(2);
        return out.good();
    }

    bool MqttRetainedStore::load_from_file(const std::string & path)
    {
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

        std::map<std::string, MqttRetainedMessage> loaded;
        for (const auto &item : root) {
            if (!item.is_object())
                continue;
            if (!item.contains("topic") || !item["topic"].is_string())
                continue;
            if (!item.contains("payload_hex") || !item["payload_hex"].is_string())
                continue;

            MqttRetainedMessage msg;
            msg.topic = item["topic"].get<std::string>();
            msg.payload = yuan::base::util::from_hex(item["payload_hex"].get<std::string>());
            const uint8_t qos_u8 = static_cast<uint8_t>(item.value("qos", 0));
            msg.qos = qos_u8 <= 2 ? static_cast<QoS>(qos_u8) : QoS::AT_MOST_ONCE;

            if (item.contains("message_expiry_interval") && item["message_expiry_interval"].is_number_unsigned())
                msg.message_expiry_interval = item["message_expiry_interval"].get<uint32_t>();
            if (item.contains("payload_format_indicator") && item["payload_format_indicator"].is_number_unsigned())
                msg.payload_format_indicator = item["payload_format_indicator"].get<uint8_t>();
            if (item.contains("content_type") && item["content_type"].is_string())
                msg.content_type = item["content_type"].get<std::string>();

            if (item.contains("user_properties") && item["user_properties"].is_array()) {
                for (const auto &up : item["user_properties"]) {
                    if (!up.is_object())
                        continue;
                    if (!up.contains("key") || !up.contains("value"))
                        continue;
                    if (!up["key"].is_string() || !up["value"].is_string())
                        continue;
                    msg.user_properties.push_back({ up["key"].get<std::string>(), up["value"].get<std::string>() });
                }
            }

            msg.stored_time = std::chrono::steady_clock::now();
            if (msg.message_expiry_interval.has_value() &&
                item.contains("stored_elapsed_seconds") &&
                item["stored_elapsed_seconds"].is_number_integer()) {
                const auto elapsed = std::max<int64_t>(0, item["stored_elapsed_seconds"].get<int64_t>());
                msg.stored_time -= std::chrono::seconds(elapsed);
            }

            if (!msg.topic.empty() && !msg.payload.empty()) {
                loaded[msg.topic] = std::move(msg);
            }
        }

        messages_.swap(loaded);
        cleanup_expired();
        return true;
    }
}
