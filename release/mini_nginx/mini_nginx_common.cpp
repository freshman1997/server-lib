#include "mini_nginx_common.h"

namespace mini_nginx
{
    int read_env_int(const char *name, int default_value)
    {
        const char *raw = std::getenv(name);
        if (!raw || *raw == '\0') {
            return default_value;
        }

        try {
            return std::stoi(raw);
        } catch (...) {
            return default_value;
        }
    }

    std::string read_env_string(const char *name, const std::string &default_value)
    {
        const char *raw = std::getenv(name);
        return raw ? std::string(raw) : default_value;
    }

    bool json_bool(const nlohmann::json &json, const char *key, bool fallback)
    {
        return json.contains(key) && json[key].is_boolean() ? json[key].get<bool>() : fallback;
    }

    int json_int(const nlohmann::json &json, const char *key, int fallback)
    {
        return json.contains(key) && json[key].is_number_integer() ? json[key].get<int>() : fallback;
    }

    std::string to_upper_ascii(std::string value)
    {
        for (auto &ch : value) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    std::string to_lower_ascii(std::string value)
    {
        for (auto &ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    yuan::net::ListenSchedulingMode parse_listen_scheduling_mode(const std::string &value, yuan::net::ListenSchedulingMode fallback)
    {
        const auto mode = to_lower_ascii(value);
        if (mode == "throughput" || mode == "default") {
            return yuan::net::ListenSchedulingMode::throughput;
        }

        if (mode == "affinity" || mode == "shard" || mode == "sharded") {
            return yuan::net::ListenSchedulingMode::affinity;
        }

        return fallback;
    }

    std::string normalize_extension(std::string ext)
    {
        ext = to_lower_ascii(std::move(ext));
        if (!ext.empty() && ext.front() != '.') {
            ext.insert(ext.begin(), '.');
        }
        return ext;
    }

    std::string join_methods(const std::unordered_set<std::string> &methods)
    {
        std::string out;
        for (const auto &method : methods) {
            if (!out.empty()) {
                out += ", ";
            }
            out += method;
        }
        return out;
    }

    std::string trim_ascii(std::string value)
    {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }

        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }

        return value.substr(begin, end - begin);
    }

    std::string remote_ip_from_request(yuan::net::http::HttpRequest *req)
    {
        auto *ctx = req ? req->get_context() : nullptr;
        auto *conn = ctx ? ctx->get_connection() : nullptr;
        if (!conn) {
            return {};
        }

        std::string remote = conn->get_remote_address().to_address_key();
        if (!remote.empty() && remote.front() == '[') {
            const auto close = remote.find(']');
            return close == std::string::npos ? remote : remote.substr(1, close - 1);
        }

        const auto first_colon = remote.find(':');
        if (first_colon != std::string::npos && remote.find(':', first_colon + 1) == std::string::npos) {
            remote.resize(first_colon);
        }

        return remote;
    }

    std::optional<uint32_t> parse_ipv4(std::string_view value)
    {
        uint32_t out = 0;
        std::size_t pos = 0;
        for (int part = 0; part < 4; ++part) {
            if (pos >= value.size()) {
                return std::nullopt;
            }

            uint32_t octet = 0;
            std::size_t digits = 0;
            while (pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])) != 0) {
                octet = octet * 10 + static_cast<uint32_t>(value[pos] - '0');
                if (octet > 255) {
                    return std::nullopt;
                }
                ++pos;
                ++digits;
            }

            if (digits == 0) {
                return std::nullopt;
            }

            out = (out << 8) | octet;
            if (part < 3) {
                if (pos >= value.size() || value[pos] != '.') {
                    return std::nullopt;
                }
                ++pos;
            }
        }

        return pos == value.size() ? std::optional<uint32_t>(out) : std::nullopt;
    }

    bool access_rule_matches(const std::string &remote_ip, const yuan::net::http::AccessRule &rule)
    {
        std::string value = trim_ascii(rule.value);
        if (value.empty()) {
            return false;
        }

        if (to_lower_ascii(value) == "all") {
            return true;
        }

        const auto slash = value.find('/');
        if (slash != std::string::npos) {
            const auto network = parse_ipv4(std::string_view(value).substr(0, slash));
            const auto remote = parse_ipv4(remote_ip);
            if (!network || !remote) {
                return false;
            }

            int prefix = -1;
            try {
                prefix = std::stoi(value.substr(slash + 1));
            } catch (...) {
                return false;
            }

            if (prefix < 0 || prefix > 32) {
                return false;
            }

            const uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
            return ((*remote & mask) == (*network & mask));
        }

        return remote_ip == value;
    }

    bool access_allowed(yuan::net::http::HttpRequest *req, const std::vector<yuan::net::http::AccessRule> &rules)
    {
        if (rules.empty()) {
            return true;
        }

        const std::string remote_ip = remote_ip_from_request(req);
        for (const auto &rule : rules) {
            if (access_rule_matches(remote_ip, rule)) {
                return rule.allow;
            }
        }

        return true;
    }

    bool route_match_is_regex(const nlohmann::json &route)
    {
        const auto check = [&](const char *key) {
            if (!route.contains(key) || !route[key].is_string()) {
                return false;
            }

            const auto match = to_lower_ascii(trim_ascii(route[key].get<std::string>()));
            return match == "regex" || match == "~" || match == "regex_i" || match == "~*";
        };

        return check("match") || check("location_match");
    }

    bool validate_route(const nlohmann::json &route, std::string &error)
    {
        if (!route.is_object()) {
            error = "route item must be an object";
            return false;
        }

        if (!route.contains("root") || !route["root"].is_string()) {
            error = "route.root(string) is required";
            return false;
        }

        const auto root = route["root"].get<std::string>();
        const bool regex_match = route_match_is_regex(route);
        if (root.empty() || (root.front() != '/' && !regex_match)) {
            error = regex_match ? "route.root regex must not be empty" : "route.root must start with '/'";
            return false;
        }

        if (!route.contains("target") || !route["target"].is_array() || route["target"].empty()) {
            error = "route.target(non-empty array) is required";
            return false;
        }

        for (const auto &upstream : route["target"]) {
            if (!upstream.is_array() || upstream.size() < 2 ||
                !upstream[0].is_string() || !upstream[1].is_number_unsigned()) {
                error = "route.target items must be [host, port]";
                return false;
            }
            
            const auto port = upstream[1].get<uint64_t>();
            if (port == 0 || port > 65535) {
                error = "route.target port must be in range [1, 65535]";
                return false;
            }
        }

        return true;
    }
}
