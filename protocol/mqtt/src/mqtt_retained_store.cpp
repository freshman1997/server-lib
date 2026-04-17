#include "mqtt_retained_store.h"
#include <algorithm>
#include <chrono>
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
}
