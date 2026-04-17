#include "mqtt_topic_tree.h"
#include <algorithm>

namespace yuan::net::mqtt
{
    static std::vector<std::string> split_topic(const std::string & topic)
    {
        std::vector<std::string> levels;
        size_t start = 0;
        size_t pos;
        while ((pos = topic.find('/', start)) != std::string::npos) {
            levels.push_back(topic.substr(start, pos - start));
            start = pos + 1;
        }
        levels.push_back(topic.substr(start));
        return levels;
    }

    MqttTopicTree::Node *MqttTopicTree::find_or_create_node(const std::string & topic_filter)
    {
        auto levels = split_topic(topic_filter);
        Node *node = &root_;
        for (const auto &level : levels) {
            if (level == "+") {
                if (!node->single_level_wildcard)
                    node->single_level_wildcard = std::make_unique<Node>();
                node = node->single_level_wildcard.get();
            } else if (level == "#") {
                if (!node->multi_level_wildcard)
                    node->multi_level_wildcard = std::make_unique<Node>();
                node = node->multi_level_wildcard.get();
            } else {
                auto it = node->children.find(level);
                if (it == node->children.end())
                    it = node->children.emplace(level, Node{}).first;
                node = &it->second;
            }
        }
        return node;
    }

    std::optional<QoS> MqttTopicTree::subscribe(const std::string & topic_filter, const MqttSubscription & sub)
    {
        std::string actual_filter = topic_filter;
        if (is_shared_subscription(topic_filter))
            actual_filter = shared_topic_filter(topic_filter);

        Node *node = find_or_create_node(actual_filter);

        for (auto &existing : node->subscriptions) {
            if (existing.session_id == sub.session_id) {
                QoS old_qos = existing.qos;
                existing = sub;
                return old_qos;
            }
        }

        node->subscriptions.push_back(sub);
        return std::nullopt;
    }

    bool MqttTopicTree::unsubscribe(const std::string & topic_filter, uint64_t session_id)
    {
        std::string actual_filter = topic_filter;
        if (is_shared_subscription(topic_filter))
            actual_filter = shared_topic_filter(topic_filter);

        auto levels = split_topic(actual_filter);
        Node *node = &root_;
        for (const auto &level : levels) {
            if (level == "+") {
                if (!node->single_level_wildcard)
                    return false;
                node = node->single_level_wildcard.get();
            } else if (level == "#") {
                if (!node->multi_level_wildcard)
                    return false;
                node = node->multi_level_wildcard.get();
            } else {
                auto it = node->children.find(level);
                if (it == node->children.end())
                    return false;
                node = &it->second;
            }
        }

        auto &subs = node->subscriptions;
        for (auto it = subs.begin(); it != subs.end(); ++it) {
            if (it->session_id == session_id) {
                subs.erase(it);
                return true;
            }
        }
        return false;
    }

    std::vector<MqttSubscription> MqttTopicTree::match(const std::string & topic) const
    {
        auto levels = split_topic(topic);
        std::vector<MqttSubscription> result;

        if (levels.empty())
            return result;

        bool is_dollar_topic = !levels[0].empty() && levels[0][0] == '$';

        if (is_dollar_topic) {
            auto it = root_.children.find(levels[0]);
            if (it != root_.children.end())
                match_recursive(it->second, levels, 1, result);
        } else {
            match_recursive(root_, levels, 0, result);
        }

        return result;
    }

    void MqttTopicTree::match_recursive(const Node & node, const std::vector<std::string> & levels,
                                        size_t level_index, std::vector<MqttSubscription> & result) const
    {
        if (level_index == levels.size()) {
            result.insert(result.end(), node.subscriptions.begin(), node.subscriptions.end());
            if (node.multi_level_wildcard)
                result.insert(result.end(), node.multi_level_wildcard->subscriptions.begin(),
                              node.multi_level_wildcard->subscriptions.end());
            return;
        }

        const std::string &current = levels[level_index];

        auto it = node.children.find(current);
        if (it != node.children.end())
            match_recursive(it->second, levels, level_index + 1, result);

        if (node.single_level_wildcard)
            match_recursive(*node.single_level_wildcard, levels, level_index + 1, result);

        if (node.multi_level_wildcard)
            result.insert(result.end(), node.multi_level_wildcard->subscriptions.begin(),
                          node.multi_level_wildcard->subscriptions.end());
    }

    std::vector<std::string> MqttTopicTree::subscriptions(uint64_t session_id) const
    {
        std::vector<std::string> result;
        std::vector<std::pair<const Node *, std::string> > stack;
        stack.emplace_back(&root_, std::string{});

        while (!stack.empty()) {
            auto entry = std::move(stack.back());
            stack.pop_back();
            const Node *node = entry.first;
            std::string path = std::move(entry.second);

            for (const auto &sub : node->subscriptions) {
                if (sub.session_id == session_id)
                    result.push_back(path);
            }

            for (const auto &pair : node->children) {
                std::string child_path = path.empty() ? pair.first : path + "/" + pair.first;
                stack.emplace_back(&pair.second, std::move(child_path));
            }

            if (node->multi_level_wildcard) {
                std::string child_path = path.empty() ? "#" : path + "/#";
                stack.emplace_back(node->multi_level_wildcard.get(), std::move(child_path));
            }

            if (node->single_level_wildcard) {
                std::string child_path = path.empty() ? "+" : path + "/+";
                stack.emplace_back(node->single_level_wildcard.get(), std::move(child_path));
            }
        }

        return result;
    }

    void MqttTopicTree::remove_all(uint64_t session_id)
    {
        std::vector<Node *> stack;
        stack.push_back(&root_);

        while (!stack.empty()) {
            Node *node = stack.back();
            stack.pop_back();

            auto &subs = node->subscriptions;
            subs.erase(
                std::remove_if(subs.begin(), subs.end(),
                               [session_id](const MqttSubscription &sub) { return sub.session_id == session_id; }),
                subs.end());

            for (auto &pair : node->children)
                stack.push_back(&pair.second);

            if (node->single_level_wildcard)
                stack.push_back(node->single_level_wildcard.get());

            if (node->multi_level_wildcard)
                stack.push_back(node->multi_level_wildcard.get());
        }
    }

    bool MqttTopicTree::validate_topic_filter(const std::string & filter)
    {
        if (filter.empty())
            return false;

        for (char c : filter) {
            if (c == '\0')
                return false;
        }

        auto levels = split_topic(filter);
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto &level = levels[i];
            if (level == "+")
                continue;
            if (level.find('+') != std::string::npos)
                return false;
            if (level == "#") {
                if (i != levels.size() - 1)
                    return false;
                continue;
            }
            if (level.find('#') != std::string::npos)
                return false;
        }

        return true;
    }

    bool MqttTopicTree::validate_topic_name(const std::string & topic)
    {
        if (topic.empty())
            return false;

        for (char c : topic) {
            if (c == '+' || c == '#' || c == '\0')
                return false;
        }

        return true;
    }

    bool MqttTopicTree::is_shared_subscription(const std::string & filter)
    {
        return filter.size() > 7 && filter.compare(0, 7, "$share/") == 0;
    }

    std::string MqttTopicTree::shared_group(const std::string & filter)
    {
        if (!is_shared_subscription(filter))
            return {};
        size_t second_slash = filter.find('/', 7);
        if (second_slash == std::string::npos)
            return {};
        return filter.substr(7, second_slash - 7);
    }

    std::string MqttTopicTree::shared_topic_filter(const std::string & filter)
    {
        if (!is_shared_subscription(filter))
            return {};
        size_t second_slash = filter.find('/', 7);
        if (second_slash == std::string::npos)
            return {};
        return filter.substr(second_slash + 1);
    }
}
