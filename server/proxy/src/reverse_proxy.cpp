#include <cassert>
#include <cstdlib>
#include <ctime>
#include <memory>
#include "logger.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_set>

#include "base/time.h"

#include "net/connection/connection_factory.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "ops/option.h"
#include "ops/config_manager.h"
#include "reverse_proxy.h"
#include "response_code.h"
#include "nlohmann/json.hpp"
#include "context.h"
#include "request.h"
#include "response.h"
#include "http_server.h"
#include "timer/timer.h"
#include "coroutine/connect_awaitable.h"
#include "net/async/async_connection_context.h"

namespace yuan::net::http
{
    namespace
    {
        static uint64_t now_ms_steady()
        {
            return base::time::steady_now_ms();
        }

        const char *connect_result_text(yuan::coroutine::ConnectResult result)
        {
            using Result = yuan::coroutine::ConnectResult;
            switch (result) {
            case Result::success:
                return "success";
            case Result::invalid_address:
                return "invalid_address";
            case Result::socket_error:
                return "socket_error";
            case Result::connect_failed:
                return "connect_failed";
            case Result::timed_out:
                return "timed_out";
            case Result::connection_error:
                return "connection_error";
            default:
                return "unknown";
            }
        }

        void replace_all(std::string &value, const std::string &from, const std::string &to)
        {
            if (from.empty()) {
                return;
            }
            std::size_t pos = 0;
            while ((pos = value.find(from, pos)) != std::string::npos) {
                value.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        std::string expand_proxy_header_value(HttpRequest *req,
                                              std::string value,
                                              const std::string &original_host,
                                              const std::string &remote_addr,
                                              const std::string &proxy_add_x_forwarded_for)
        {
            const std::string request_uri = req ? req->get_raw_url() : "/";
            const std::string uri = req ? std::string(req->get_path()) : "/";
            replace_all(value, "$host", original_host);
            replace_all(value, "$http_host", original_host);
            replace_all(value, "$remote_addr", remote_addr);
            replace_all(value, "$proxy_add_x_forwarded_for", proxy_add_x_forwarded_for);
            replace_all(value, "$scheme", "http");
            replace_all(value, "$request_uri", request_uri.empty() ? "/" : request_uri);
            replace_all(value, "$uri", uri.empty() ? "/" : uri);
            return value;
        }

        void append_header_map(const nlohmann::json &obj,
                               std::vector<std::pair<std::string, std::string>> &headers)
        {
            if (!obj.is_object()) {
                return;
            }
            for (auto it = obj.begin(); it != obj.end(); ++it) {
                if (it.key().empty() || !it.value().is_string()) {
                    continue;
                }
                headers.emplace_back(it.key(), it.value().get<std::string>());
            }
        }

        void append_proxy_redirects(const nlohmann::json &value,
                                    std::vector<ProxyRedirectRule> &redirects)
        {
            auto append_one = [&](const nlohmann::json &item) {
                if (!item.is_object()) {
                    return;
                }
                if (!item.contains("from") || !item["from"].is_string() ||
                    !item.contains("to") || !item["to"].is_string()) {
                    return;
                }
                ProxyRedirectRule rule;
                rule.from = item["from"].get<std::string>();
                rule.to = item["to"].get<std::string>();
                if (!rule.from.empty() && rule.from.find_first_of("\r\n") == std::string::npos &&
                    rule.to.find_first_of("\r\n") == std::string::npos) {
                    redirects.push_back(std::move(rule));
                }
            };

            if (value.is_array()) {
                for (const auto &item : value) {
                    append_one(item);
                }
                return;
            }
            if (value.is_object()) {
                if (value.contains("from") || value.contains("to")) {
                    append_one(value);
                    return;
                }
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (!it.value().is_string()) {
                        continue;
                    }
                    ProxyRedirectRule rule;
                    rule.from = it.key();
                    rule.to = it.value().get<std::string>();
                    if (!rule.from.empty() && rule.from.find_first_of("\r\n") == std::string::npos &&
                        rule.to.find_first_of("\r\n") == std::string::npos) {
                        redirects.push_back(std::move(rule));
                    }
                }
            }
        }

        void append_string_array(const nlohmann::json &arr, std::vector<std::string> &values)
        {
            if (!arr.is_array()) {
                return;
            }
            for (const auto &item : arr) {
                if (item.is_string()) {
                    values.push_back(item.get<std::string>());
                }
            }
        }

        void append_method_set(const nlohmann::json &arr, std::unordered_set<std::string> &methods)
        {
            if (!arr.is_array()) {
                return;
            }
            for (const auto &item : arr) {
                if (!item.is_string()) {
                    continue;
                }
                std::string method = item.get<std::string>();
                std::transform(method.begin(), method.end(), method.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::toupper(ch));
                });
                if (!method.empty()) {
                    methods.insert(std::move(method));
                }
            }
        }

        void append_access_values(const nlohmann::json &value,
                                  bool allow,
                                  std::vector<AccessRule> &rules)
        {
            if (value.is_string()) {
                AccessRule rule;
                rule.allow = allow;
                rule.value = value.get<std::string>();
                rules.push_back(std::move(rule));
                return;
            }
            if (!value.is_array()) {
                return;
            }
            for (const auto &item : value) {
                if (!item.is_string()) {
                    continue;
                }
                AccessRule rule;
                rule.allow = allow;
                rule.value = item.get<std::string>();
                rules.push_back(std::move(rule));
            }
        }

        std::string lower_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        std::string trim_ascii_copy(std::string value)
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

        std::string request_path_for_route_lookup(const std::string &url)
        {
            const auto query = url.find('?');
            return query == std::string::npos ? url : url.substr(0, query);
        }

        const char *match_type_name(ProxyRoute::MatchType type)
        {
            switch (type) {
            case ProxyRoute::MatchType::exact:
                return "exact";
            case ProxyRoute::MatchType::prefix_strong:
                return "^~";
            case ProxyRoute::MatchType::regex:
                return "regex";
            case ProxyRoute::MatchType::prefix:
            default:
                return "prefix";
            }
        }

        void erase_route_key(std::vector<std::string> &keys, const std::string &key)
        {
            keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
        }

        void apply_match_type(const nlohmann::json &obj, ProxyRoute &route)
        {
            if (obj.contains("exact") && obj["exact"].is_boolean() && obj["exact"].get<bool>()) {
                route.match_type = ProxyRoute::MatchType::exact;
            }
            const auto apply_string = [&](const char *key) {
                if (!obj.contains(key) || !obj[key].is_string()) {
                    return;
                }
                const auto match = lower_ascii(trim_ascii_copy(obj[key].get<std::string>()));
                if (match == "exact" || match == "=") {
                    route.match_type = ProxyRoute::MatchType::exact;
                } else if (match == "prefix" || match == "prefix_strong" || match == "^~") {
                    route.match_type = ProxyRoute::MatchType::prefix;
                    if (match == "prefix_strong" || match == "^~") {
                        route.match_type = ProxyRoute::MatchType::prefix_strong;
                    }
                } else if (match == "regex" || match == "~") {
                    route.match_type = ProxyRoute::MatchType::regex;
                    route.regex_case_sensitive = true;
                    route.strip_prefix = false;
                } else if (match == "regex_i" || match == "~*") {
                    route.match_type = ProxyRoute::MatchType::regex;
                    route.regex_case_sensitive = false;
                    route.strip_prefix = false;
                }
            };
            apply_string("match");
            apply_string("location_match");
        }

        std::string header_name_lower(std::string_view line)
        {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                return {};
            }
            return lower_ascii(trim_ascii_copy(std::string(line.substr(0, colon))));
        }

        bool route_has_response_header_rules(const ProxyRoute &route)
        {
            return !route.response_headers.empty() || !route.hide_response_headers.empty();
        }

        std::string upper_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::toupper(ch));
            });
            return value;
        }

        std::string join_method_set(const std::unordered_set<std::string> &methods)
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

        bool reject_method_if_not_allowed(HttpRequest *req,
                                          HttpResponse *resp,
                                          const std::unordered_set<std::string> &allowed_methods)
        {
            if (!req || !resp || allowed_methods.empty()) {
                return false;
            }
            const std::string method = upper_ascii(req->get_raw_method());
            if (allowed_methods.find(method) != allowed_methods.end()) {
                return false;
            }

            const std::string body = "405 Method Not Allowed\n";
            resp->set_response_code(ResponseCode::method_not_allowed);
            resp->add_header("Allow", join_method_set(allowed_methods));
            resp->add_header("Content-Type", "text/plain; charset=utf-8");
            resp->append_body(body);
            resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
            resp->send();
            return true;
        }

        std::string remote_ip_from_request(HttpRequest *req)
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

        bool access_rule_matches(const std::string &remote_ip, const AccessRule &rule)
        {
            std::string value = trim_ascii_copy(rule.value);
            if (value.empty()) {
                return false;
            }
            if (lower_ascii(value) == "all") {
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

        bool access_allowed(HttpRequest *req, const std::vector<AccessRule> &rules)
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

        bool reject_if_access_denied(HttpRequest *req, HttpResponse *resp, const std::vector<AccessRule> &rules)
        {
            if (!req || !resp || access_allowed(req, rules)) {
                return false;
            }
            resp->process_error(ResponseCode::forbidden);
            return true;
        }

        std::string apply_proxy_redirects(std::string_view line, const ProxyRoute &route)
        {
            if (route.proxy_redirects.empty()) {
                return std::string(line);
            }
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                return std::string(line);
            }
            const auto name = lower_ascii(trim_ascii_copy(std::string(line.substr(0, colon))));
            if (name != "location" && name != "refresh") {
                return std::string(line);
            }

            std::string value = trim_ascii_copy(std::string(line.substr(colon + 1)));
            for (const auto &rule : route.proxy_redirects) {
                const auto pos = value.find(rule.from);
                if (pos == std::string::npos) {
                    continue;
                }
                value.replace(pos, rule.from.size(), rule.to);
                std::string out(line.substr(0, colon));
                out.append(": ");
                out.append(value);
                return out;
            }
            return std::string(line);
        }

        std::string rewrite_proxy_response_headers(std::string_view data, const ProxyRoute &route)
        {
            const auto header_end = data.find("\r\n\r\n");
            if (header_end == std::string_view::npos) {
                return std::string(data);
            }

            std::unordered_set<std::string> remove_names;
            for (const auto &name : route.hide_response_headers) {
                remove_names.insert(lower_ascii(trim_ascii_copy(name)));
            }
            for (const auto &header : route.response_headers) {
                remove_names.insert(lower_ascii(trim_ascii_copy(header.first)));
            }

            std::string out;
            out.reserve(data.size() + 128);

            std::size_t line_begin = 0;
            const auto status_end = data.find("\r\n");
            if (status_end == std::string_view::npos || status_end > header_end) {
                return std::string(data);
            }
            out.append(data.substr(0, status_end + 2));
            line_begin = status_end + 2;

            while (line_begin < header_end) {
                const auto line_end = data.find("\r\n", line_begin);
                if (line_end == std::string_view::npos || line_end > header_end) {
                    break;
                }
                const auto line = data.substr(line_begin, line_end - line_begin);
                const auto name = header_name_lower(line);
                if (name.empty() || remove_names.find(name) == remove_names.end()) {
                    out.append(apply_proxy_redirects(line, route));
                    out.append("\r\n");
                }
                line_begin = line_end + 2;
            }

            for (const auto &header : route.response_headers) {
                if (header.first.empty() || header.second.empty()) {
                    continue;
                }
                out.append(header.first);
                out.append(": ");
                out.append(header.second);
                out.append("\r\n");
            }

            out.append("\r\n");
            out.append(data.substr(header_end + 4));
            return out;
        }

        int parse_proxy_status_code(std::string_view data)
        {
            const auto line_end = data.find("\r\n");
            if (line_end == std::string_view::npos) {
                return 0;
            }
            const auto first_space = data.find(' ');
            if (first_space == std::string_view::npos || first_space + 3 > line_end) {
                return 0;
            }
            int code = 0;
            for (std::size_t i = 0; i < 3; ++i) {
                const char ch = data[first_space + 1 + i];
                if (!std::isdigit(static_cast<unsigned char>(ch))) {
                    return 0;
                }
                code = code * 10 + (ch - '0');
            }
            return code;
        }

        std::optional<std::size_t> parse_proxy_content_length(std::string_view data)
        {
            const auto header_end = data.find("\r\n\r\n");
            if (header_end == std::string_view::npos) {
                return std::nullopt;
            }
            std::size_t line_begin = data.find("\r\n");
            if (line_begin == std::string_view::npos || line_begin >= header_end) {
                return std::nullopt;
            }
            line_begin += 2;
            while (line_begin < header_end) {
                const auto line_end = data.find("\r\n", line_begin);
                if (line_end == std::string_view::npos || line_end > header_end) {
                    break;
                }
                const auto line = data.substr(line_begin, line_end - line_begin);
                if (header_name_lower(line) == "content-length") {
                    const auto colon = line.find(':');
                    if (colon == std::string_view::npos) {
                        return std::nullopt;
                    }
                    const auto value = trim_ascii_copy(std::string(line.substr(colon + 1)));
                    try {
                        return static_cast<std::size_t>(std::stoull(value));
                    } catch (...) {
                        return std::nullopt;
                    }
                }
                line_begin = line_end + 2;
            }
            return std::nullopt;
        }

        std::string proxy_header_value(std::string_view data, std::string_view header_name)
        {
            const auto header_end = data.find("\r\n\r\n");
            if (header_end == std::string_view::npos) {
                return {};
            }
            const std::string expected = lower_ascii(trim_ascii_copy(std::string(header_name)));
            std::size_t line_begin = data.find("\r\n");
            if (line_begin == std::string_view::npos || line_begin >= header_end) {
                return {};
            }
            line_begin += 2;
            while (line_begin < header_end) {
                const auto line_end = data.find("\r\n", line_begin);
                if (line_end == std::string_view::npos || line_end > header_end) {
                    break;
                }
                const auto line = data.substr(line_begin, line_end - line_begin);
                if (header_name_lower(line) == expected) {
                    const auto colon = line.find(':');
                    return colon == std::string_view::npos
                               ? std::string{}
                               : trim_ascii_copy(std::string(line.substr(colon + 1)));
                }
                line_begin = line_end + 2;
            }
            return {};
        }

        std::string set_proxy_cache_header(std::string_view data, std::string_view value)
        {
            const auto header_end = data.find("\r\n\r\n");
            if (header_end == std::string_view::npos) {
                return std::string(data);
            }

            std::string out;
            out.reserve(data.size() + value.size() + 16);
            const auto status_end = data.find("\r\n");
            if (status_end == std::string_view::npos || status_end > header_end) {
                return std::string(data);
            }

            out.append(data.substr(0, status_end + 2));
            std::size_t line_begin = status_end + 2;
            while (line_begin < header_end) {
                const auto line_end = data.find("\r\n", line_begin);
                if (line_end == std::string_view::npos || line_end > header_end) {
                    break;
                }
                const auto line = data.substr(line_begin, line_end - line_begin);
                if (header_name_lower(line) != "x-cache") {
                    out.append(line);
                    out.append("\r\n");
                }
                line_begin = line_end + 2;
            }
            out.append("X-Cache: ");
            out.append(value);
            out.append("\r\n\r\n");
            out.append(data.substr(header_end + 4));
            return out;
        }

        bool proxy_cache_payload_complete(const std::string &payload,
                                          const ProxyRoute &route)
        {
            if (payload.size() > route.cache_max_response_bytes) {
                return false;
            }
            const auto header_end = payload.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                return false;
            }
            if (parse_proxy_status_code(payload) != 200) {
                return false;
            }
            const auto content_length = parse_proxy_content_length(payload);
            if (!content_length) {
                return false;
            }
            if (!route.cache_ignore_set_cookie && !proxy_header_value(payload, "set-cookie").empty()) {
                return false;
            }
            if (!route.cache_ignore_cache_control) {
                const auto cache_control = lower_ascii(proxy_header_value(payload, "cache-control"));
                if (cache_control.find("no-store") != std::string::npos ||
                    cache_control.find("private") != std::string::npos) {
                    return false;
                }
            }
            return payload.size() >= header_end + 4 + *content_length;
        }

        bool request_has_any_header(HttpRequest *req, const std::vector<std::string> &headers)
        {
            if (!req) {
                return false;
            }
            for (const auto &name : headers) {
                if (name.empty()) {
                    continue;
                }
                const auto *value = req->get_header(name);
                if (value && !value->empty()) {
                    return true;
                }
            }
            return false;
        }

        std::string expand_proxy_cache_key(HttpRequest *req,
                                           const std::string &route_key,
                                           std::string value)
        {
            const std::string request_uri = req ? req->get_raw_url() : "/";
            const std::string uri = req ? std::string(req->get_path()) : "/";
            std::string host;
            if (req) {
                if (const auto *header = req->get_header("host")) {
                    host = *header;
                }
            }
            replace_all(value, "$scheme", "http");
            replace_all(value, "$host", host);
            replace_all(value, "$http_host", host);
            replace_all(value, "$request_uri", request_uri);
            replace_all(value, "$uri", uri);
            replace_all(value, "$route", route_key);
            return value;
        }

        std::string make_proxy_cache_key(HttpRequest *req,
                                         const ProxyRoute &route,
                                         const std::string &route_key,
                                         const std::string &url)
        {
            if (!route.cache_key_template.empty()) {
                return expand_proxy_cache_key(req, route_key, route.cache_key_template);
            }
            std::string host;
            if (req) {
                if (const auto *header = req->get_header("host")) {
                    host = *header;
                }
            }
            return route_key + "\n" + host + "\n" + url;
        }
    }


    bool PooledConnection::is_expired(uint64_t max_idle_ms) const
    {
        const auto now_ms = base::time::steady_now_ms();
        return !in_use.load(std::memory_order_relaxed) &&
               now_ms > last_used_ms + max_idle_ms;
    }

    void PooledConnection::mark_used()
    {
        in_use.store(true, std::memory_order_relaxed);
        ++ref_count;
        last_used_ms = base::time::steady_now_ms();
    }

    TargetConnectionPool::TargetConnectionPool(const ProxyTarget & target, size_t max_size,
                                               std::chrono::seconds idle_timeout)
        : target_(target), max_size_(max_size), idle_timeout_(idle_timeout)
    {
    }

    TargetConnectionPool::~TargetConnectionPool()
    {
        close_all();
    }

    Connection *TargetConnectionPool::acquire(HttpProxy * proxy, HttpServer * server)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        size_t start = rr_index_.load(std::memory_order_relaxed);
        size_t count = connections_.size();

        for (size_t i = 0; i < count; ++i) {
            size_t idx = (start + i) % count;
            auto &pc = connections_[idx];

            bool expected = false;
            if (!pc.in_use.load(std::memory_order_acquire) &&
                pc.in_use.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel, std::memory_order_relaxed)) {
                pc.mark_used();
                rr_index_.store((idx + 1) % count, std::memory_order_relaxed);
                return pc.conn;
            }
        }

        return nullptr;
    }

    void TargetConnectionPool::release(Connection * conn)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto &pc : connections_) {
            if (pc.conn == conn && pc.in_use.load(std::memory_order_relaxed)) {
                --pc.ref_count;
                pc.in_use.store(false, std::memory_order_release);
                pc.last_used_ms = base::time::steady_now_ms();
                return;
            }
        }
    }
    void TargetConnectionPool::remove(Connection * conn)
    {
        Connection *to_close = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = connections_.begin(); it != connections_.end(); ++it) {
                if (it->conn == conn) {
                    to_close = it->conn;
                    connections_.erase(it);
                    break;
                }
            }
        }
        if (to_close) {
            to_close->close();
        }
    }

    size_t TargetConnectionPool::cleanup_idle()
    {
        size_t removed = 0;
        const auto now_ms = base::time::steady_now_ms();
        std::vector<Connection *> to_close;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (!it->in_use.load(std::memory_order_relaxed) &&
                    now_ms > it->last_used_ms + static_cast<uint64_t>(idle_timeout_.count()) * 1000ULL) {
                    if (it->conn) {
                        to_close.push_back(it->conn);
                    }
                    it = connections_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }

            if (!connections_.empty()) {
                rr_index_.store(0, std::memory_order_relaxed);
            }
        }

        for (auto *conn : to_close) {
            conn->close();
        }

        return removed;
    }

    void TargetConnectionPool::close_all()
    {
        std::vector<Connection *> to_close;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &pc : connections_) {
                if (pc.conn) {
                    to_close.push_back(pc.conn);
                }
            }
            connections_.clear();
            rr_index_.store(0, std::memory_order_relaxed);
        }
        for (auto *conn : to_close) {
            conn->close();
        }
    }

    size_t TargetConnectionPool::active_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t n = 0;
        for (const auto &pc : connections_) {
            if (pc.in_use.load(std::memory_order_relaxed))
                ++n;
        }
        return n;
    }

    size_t TargetConnectionPool::total_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

    size_t TargetConnectionPool::available_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t active = 0;
        for (const auto &pc : connections_) {
            if (pc.in_use.load(std::memory_order_relaxed)) {
                ++active;
            }
        }
        return connections_.size() - active;
    }

    Connection *TargetConnectionPool::create_new_connection(HttpProxy * proxy, HttpServer * server)
    {
        if (!server || !proxy)
            return nullptr;

        auto *runtime = server->runtime();
        if (!runtime)
            return nullptr;

        auto sock = new net::Socket(target_.host.c_str(), target_.port);
        if (!sock->valid()) {
            delete sock;
            return nullptr;
        }

        auto conn = create_stream_connection(sock);
        runtime->register_connection(conn, make_non_owning_handler(proxy));

        PooledConnection pc;
        pc.conn = &*conn;
        pc.created_at_ms = base::time::steady_now_ms();
        pc.mark_used();

        connections_.push_back(std::move(pc));
        return &*conn;
    }

    HttpProxy::HttpProxy()
    {
        rng_.seed(static_cast<unsigned>(base::time::system_now_seconds()));
    }

    HttpProxy::HttpProxy(HttpServer * server)
        : server_(server)
    {
        rng_.seed(static_cast<unsigned>(base::time::system_now_seconds()));
    }

    HttpProxy::~HttpProxy()
    {
        std::vector<Connection *> mapped_connections;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            for (auto &pair : sc_mapping_) {
                if (pair.first)
                    mapped_connections.push_back(pair.first);
                if (pair.second.client_conn)
                    mapped_connections.push_back(pair.second.client_conn);
            }
        }

        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            for (auto &pair : pools_) {
                if (pair.second) {
                    pool_snapshot.push_back(pair.second);
                }
            }
        }

        for (auto &pool : pool_snapshot) {
            pool->close_all();
        }

        for (auto *conn : mapped_connections) {
            conn->close();
        }

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            sc_mapping_.clear();
            cs_mapping_.clear();
            pending_requests_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pools_.clear();
        }
    }

    void HttpProxy::on_connected(Connection &conn)
    {
        (void)conn;
    }

    void HttpProxy::on_error(Connection &conn)
    {
        (void)unmap_and_close_peer(&conn, false);
    }

    void HttpProxy::on_read(Connection &conn)
    {
        auto *conn_ptr = &conn;
        ServerMapping mapping;
        bool is_server = false;

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);

            auto server_it = sc_mapping_.find(conn_ptr);
            if (server_it != sc_mapping_.end()) {
                mapping = server_it->second;
                is_server = true;
            } else {
                auto client_it = cs_mapping_.find(conn_ptr);
                if (client_it != cs_mapping_.end() && client_it->second) {
                    forward_data(conn_ptr, client_it->second);
                    client_it->second->flush();
                    return;
                } else {
                    return;
                }
            }
        }

        if (is_server && mapping.client_conn) {
            ProxyRoute route;
            bool have_route = false;
            {
                std::lock_guard<std::mutex> route_lock(route_mutex_);
                auto route_it = routes_.find(mapping.route_key);
                if (route_it != routes_.end()) {
                    route = route_it->second;
                    have_route = true;
                }
            }

            if (!have_route ||
                (!route_has_response_header_rules(route) &&
                 !mapping.cache_candidate &&
                 mapping.cache_status.empty())) {
                forward_data(conn_ptr, mapping.client_conn);
            } else {
                constexpr std::size_t kMaxProxyResponseHeaderBytes = 64 * 1024;
                const auto input = conn_ptr->take_input_byte_buffer();
                if (!input.empty()) {
                    std::string out;
                    std::string cache_key_to_store;
                    std::string cache_payload_to_store;
                    int cache_ttl_ms = 0;
                    {
                        std::lock_guard<std::mutex> lock(mapping_mutex_);
                        auto server_it = sc_mapping_.find(conn_ptr);
                        if (server_it == sc_mapping_.end()) {
                            return;
                        }
                        auto &state = server_it->second;
                        if (state.response_header_done) {
                            out.assign(input.read_ptr(), input.readable_bytes());
                        } else {
                            state.response_header_buffer.append(input.read_ptr(), input.readable_bytes());
                            const auto header_end = state.response_header_buffer.find("\r\n\r\n");
                            if (header_end != std::string::npos) {
                                out = rewrite_proxy_response_headers(state.response_header_buffer, route);
                                if (!state.cache_status.empty()) {
                                    out = set_proxy_cache_header(out, state.cache_status);
                                }
                                state.response_header_buffer.clear();
                                state.response_header_done = true;
                            } else if (state.response_header_buffer.size() > kMaxProxyResponseHeaderBytes) {
                                out = std::move(state.response_header_buffer);
                                state.response_header_buffer.clear();
                                state.response_header_done = true;
                                state.cache_candidate = false;
                            }
                        }
                        if (state.cache_candidate && !out.empty()) {
                            if (state.cache_buffer.size() + out.size() > route.cache_max_response_bytes) {
                                state.cache_candidate = false;
                                state.cache_buffer.clear();
                            } else {
                                state.cache_buffer.append(out);
                                const auto header_end = state.cache_buffer.find("\r\n\r\n");
                                if (header_end != std::string::npos &&
                                    parse_proxy_status_code(state.cache_buffer) != 200) {
                                    state.cache_candidate = false;
                                    state.cache_buffer.clear();
                                } else if (proxy_cache_payload_complete(state.cache_buffer, route)) {
                                    cache_key_to_store = state.cache_key;
                                    cache_payload_to_store = state.cache_buffer;
                                    cache_ttl_ms = route.cache_ttl_ms;
                                    state.cache_candidate = false;
                                    state.cache_buffer.clear();
                                }
                            }
                        }
                    }
                    if (!cache_key_to_store.empty() && cache_ttl_ms > 0) {
                        std::lock_guard<std::mutex> lock(cache_mutex_);
                        response_cache_[cache_key_to_store] = CachedProxyResponse{
                            std::move(cache_payload_to_store),
                            now_ms_steady() + static_cast<uint64_t>(cache_ttl_ms)
                        };
                    }
                    if (!out.empty()) {
                        mapping.client_conn->write(::yuan::buffer::ByteBuffer(std::string_view(out)));
                    }
                }
            }
            mapping.client_conn->flush();
        }
    }

    void HttpProxy::on_write(Connection &conn)
    {
        (void)conn;
    }

    void HttpProxy::on_close(Connection &conn)
    {
        auto *conn_ptr = &conn;

        bool is_server = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            is_server = (sc_mapping_.find(conn_ptr) != sc_mapping_.end());
        }

        const bool unmapped = unmap_and_close_peer(conn_ptr, is_server);
        if (!unmapped) {
            ++stats_.unmapped_close_events;
            LOG_DEBUG_TAG("on_close",
                          "[Proxy] close without mapping peer={} side={}",
                          conn_ptr->get_remote_address().to_address_key(),
                          is_server ? "server" : "client");
        }
        remove_connection_from_pools(conn_ptr);
    }

    bool HttpProxy::load_proxy_config_and_init()
    {
        auto cfgManager = HttpConfigManager::get_instance();
        if (!cfgManager || !cfgManager->good()) {
            LOG_WARN_TAG("load_proxy_config_and_init", "[Proxy] config manager not available");
            return false;
        }

        const auto &proxiesCfg = cfgManager->get_type_array_properties<nlohmann::json>("proxies");
        if (proxiesCfg.empty()) {
            return false;
        }

        assert(server_);
        LOG_INFO_TAG("load_proxy_config_and_init", "[Proxy] loading proxy configs...");

        for (const auto &proxyCfg : proxiesCfg) {
            if (!proxyCfg.is_object()) {
                continue;
            }

            ProxyRoute route;

            if (proxyCfg.contains("root") && proxyCfg["root"].is_string()) {
                route.match_pattern = proxyCfg["root"].get<std::string>();
            } else {
                continue;
            }
            apply_match_type(proxyCfg, route);

            if (proxyCfg.contains("target") && proxyCfg["target"].is_array()) {
                for (const auto &t : proxyCfg["target"]) {
                    if (!t.is_array() || t.size() < 2 || !t[0].is_string() || !t[1].is_number_unsigned()) {
                        continue;
                    }

                    ProxyTarget tgt;
                    tgt.host = t[0].get<std::string>();
                    tgt.port = t[1].get<uint16_t>();
                    if (proxyCfg.contains("weight") && proxyCfg["weight"].is_number()) {
                        tgt.weight = proxyCfg["weight"].get<int>();
                    }
                    route.targets.push_back(tgt);
                }
            }

            if (route.targets.empty()) {
                LOG_WARN_TAG("load_proxy_config_and_init", "[Proxy] route '{}' has no valid targets, skipping", route.match_pattern);
                continue;
            }

            if (proxyCfg.contains("balance") && proxyCfg["balance"].is_string()) {
                const std::string &bs = proxyCfg["balance"].get<std::string>();
                if (bs == "random") {
                    route.balance = ProxyRoute::BalanceStrategy::random;
                } else if (bs == "least_conn") {
                    route.balance = ProxyRoute::BalanceStrategy::least_connections;
                } else if (bs == "weighted_rr") {
                    route.balance = ProxyRoute::BalanceStrategy::weighted_round_robin;
                } else {
                    route.balance = ProxyRoute::BalanceStrategy::round_robin;
                }
            }

            if (proxyCfg.contains("strip_prefix") && proxyCfg["strip_prefix"].is_boolean()) {
                route.strip_prefix = proxyCfg["strip_prefix"].get<bool>();
            }
            if (proxyCfg.contains("rewrite") && proxyCfg["rewrite"].is_string()) {
                route.rewrite_prefix = proxyCfg["rewrite"].get<std::string>();
            }
            if (proxyCfg.contains("connect_timeout") && proxyCfg["connect_timeout"].is_number()) {
                route.connect_timeout_ms = proxyCfg["connect_timeout"].get<int>();
            }
            if (proxyCfg.contains("read_timeout") && proxyCfg["read_timeout"].is_number()) {
                route.read_timeout_ms = proxyCfg["read_timeout"].get<int>();
            }
            if (proxyCfg.contains("write_timeout") && proxyCfg["write_timeout"].is_number()) {
                route.write_timeout_ms = proxyCfg["write_timeout"].get<int>();
            }
            if (proxyCfg.contains("max_retries") && proxyCfg["max_retries"].is_number()) {
                route.max_retries = proxyCfg["max_retries"].get<int>();
            }
            if (proxyCfg.contains("pool_size") && proxyCfg["pool_size"].is_number_unsigned()) {
                route.max_pool_size_per_target = proxyCfg["pool_size"].get<size_t>();
            }
            if (proxyCfg.contains("idle_timeout") && proxyCfg["idle_timeout"].is_number()) {
                route.idle_timeout_seconds = proxyCfg["idle_timeout"].get<size_t>();
            }
            if (proxyCfg.contains("proxy_cache") && proxyCfg["proxy_cache"].is_boolean()) {
                route.cache_enabled = proxyCfg["proxy_cache"].get<bool>();
            }
            if (proxyCfg.contains("proxy_cache_valid") && proxyCfg["proxy_cache_valid"].is_number_integer()) {
                route.cache_ttl_ms = proxyCfg["proxy_cache_valid"].get<int>();
            }
            if (proxyCfg.contains("proxy_cache_ttl") && proxyCfg["proxy_cache_ttl"].is_number_integer()) {
                route.cache_ttl_ms = proxyCfg["proxy_cache_ttl"].get<int>();
            }
            if (proxyCfg.contains("proxy_cache_max_size") && proxyCfg["proxy_cache_max_size"].is_number_unsigned()) {
                route.cache_max_response_bytes = proxyCfg["proxy_cache_max_size"].get<size_t>();
            }
            if (proxyCfg.contains("proxy_cache_methods")) {
                std::unordered_set<std::string> methods;
                append_method_set(proxyCfg["proxy_cache_methods"], methods);
                if (!methods.empty()) {
                    route.cache_methods = std::move(methods);
                }
            }
            if (proxyCfg.contains("proxy_cache_key") && proxyCfg["proxy_cache_key"].is_string()) {
                route.cache_key_template = proxyCfg["proxy_cache_key"].get<std::string>();
            }
            if (proxyCfg.contains("proxy_cache_bypass_headers")) {
                append_string_array(proxyCfg["proxy_cache_bypass_headers"], route.cache_bypass_headers);
            }
            if (proxyCfg.contains("proxy_no_cache_headers")) {
                append_string_array(proxyCfg["proxy_no_cache_headers"], route.cache_no_cache_headers);
            }
            if (proxyCfg.contains("proxy_cache_ignore_cache_control") &&
                proxyCfg["proxy_cache_ignore_cache_control"].is_boolean()) {
                route.cache_ignore_cache_control = proxyCfg["proxy_cache_ignore_cache_control"].get<bool>();
            }
            if (proxyCfg.contains("proxy_cache_ignore_set_cookie") &&
                proxyCfg["proxy_cache_ignore_set_cookie"].is_boolean()) {
                route.cache_ignore_set_cookie = proxyCfg["proxy_cache_ignore_set_cookie"].get<bool>();
            }
            if (proxyCfg.contains("preserve_host") && proxyCfg["preserve_host"].is_boolean()) {
                route.preserve_host = proxyCfg["preserve_host"].get<bool>();
            }
            if (proxyCfg.contains("proxy_preserve_host") && proxyCfg["proxy_preserve_host"].is_boolean()) {
                route.preserve_host = proxyCfg["proxy_preserve_host"].get<bool>();
            }
            if (proxyCfg.contains("proxy_set_header")) {
                append_header_map(proxyCfg["proxy_set_header"], route.request_headers);
            }
            if (proxyCfg.contains("proxy_headers")) {
                append_header_map(proxyCfg["proxy_headers"], route.request_headers);
            }
            if (proxyCfg.contains("hide_request_headers")) {
                append_string_array(proxyCfg["hide_request_headers"], route.hide_request_headers);
            }
            if (proxyCfg.contains("proxy_hide_request_headers")) {
                append_string_array(proxyCfg["proxy_hide_request_headers"], route.hide_request_headers);
            }
            if (proxyCfg.contains("proxy_set_response_header")) {
                append_header_map(proxyCfg["proxy_set_response_header"], route.response_headers);
            }
            if (proxyCfg.contains("proxy_response_headers")) {
                append_header_map(proxyCfg["proxy_response_headers"], route.response_headers);
            }
            if (proxyCfg.contains("hide_response_headers")) {
                append_string_array(proxyCfg["hide_response_headers"], route.hide_response_headers);
            }
            if (proxyCfg.contains("proxy_hide_header")) {
                append_string_array(proxyCfg["proxy_hide_header"], route.hide_response_headers);
            }
            if (proxyCfg.contains("proxy_redirect")) {
                append_proxy_redirects(proxyCfg["proxy_redirect"], route.proxy_redirects);
            }
            if (proxyCfg.contains("proxy_redirects")) {
                append_proxy_redirects(proxyCfg["proxy_redirects"], route.proxy_redirects);
            }
            if (proxyCfg.contains("allowed_methods")) {
                append_method_set(proxyCfg["allowed_methods"], route.allowed_methods);
            }
            if (proxyCfg.contains("limit_except")) {
                append_method_set(proxyCfg["limit_except"], route.allowed_methods);
            }
            if (proxyCfg.contains("allow")) {
                append_access_values(proxyCfg["allow"], true, route.access_rules);
            }
            if (proxyCfg.contains("deny")) {
                append_access_values(proxyCfg["deny"], false, route.access_rules);
            }

            add_route(route);
        }

        return !routes_.empty();
    }

    void HttpProxy::add_route(const ProxyRoute & route)
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        ProxyRoute stored = route;
        if (stored.match_type == ProxyRoute::MatchType::regex) {
            try {
                auto flags = std::regex::ECMAScript;
                if (!stored.regex_case_sensitive) {
                    flags |= std::regex::icase;
                }
                stored.compiled_match = std::regex(stored.match_pattern, flags);
            } catch (const std::regex_error &ex) {
                LOG_ERROR_TAG("add_route",
                              "[Proxy] skip route: invalid regex pattern='{}', error='{}'",
                              stored.match_pattern,
                              ex.what());
                return;
            }
        }

        exact_route_keys_.erase(stored.match_pattern);
        erase_route_key(strong_prefix_route_keys_, stored.match_pattern);
        erase_route_key(regex_route_keys_, stored.match_pattern);

        routes_[stored.match_pattern] = stored;
        if (stored.match_type == ProxyRoute::MatchType::exact) {
            exact_route_keys_.insert(stored.match_pattern);
        } else if (stored.match_type == ProxyRoute::MatchType::regex) {
            regex_route_keys_.push_back(stored.match_pattern);
        } else {
            if (stored.match_type == ProxyRoute::MatchType::prefix_strong) {
                strong_prefix_route_keys_.push_back(stored.match_pattern);
            }
            url_trie_.insert(stored.match_pattern, true);
        }

        LOG_INFO_TAG("add_route", "[Proxy] register route: pattern='{}', match={}, targets={}, balance={}",
                     stored.match_pattern,
                     match_type_name(routes_[stored.match_pattern].match_type),
                     stored.targets.size(),
                     static_cast<int>(routes_[stored.match_pattern].balance));

        for (const auto &tgt : routes_[stored.match_pattern].targets) {
            LOG_INFO_TAG("add_route", "[Proxy] -> {}:{} (weight={})", tgt.host, tgt.port, tgt.weight);
        }
    }

    void HttpProxy::clear_routes()
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        std::lock_guard<std::mutex> lock(rr_mutex_);
        routes_.clear();
        exact_route_keys_.clear();
        strong_prefix_route_keys_.clear();
        regex_route_keys_.clear();
        rr_indices_.clear();
        url_trie_.clear();
        {
            std::lock_guard<std::mutex> cache_lock(cache_mutex_);
            response_cache_.clear();
        }
    }

    std::string HttpProxy::find_proxy_route(const std::string & url) const
    {
        std::lock_guard<std::mutex> route_lock(route_mutex_);
        const std::string path = request_path_for_route_lookup(url);
        if (exact_route_keys_.find(path) != exact_route_keys_.end()) {
            return path;
        }

        std::string best_strong_prefix;
        for (const auto &key : strong_prefix_route_keys_) {
            const auto route_it = routes_.find(key);
            if (route_it == routes_.end() ||
                route_it->second.match_type != ProxyRoute::MatchType::prefix_strong) {
                continue;
            }
            if (path.rfind(key, 0) == 0 && key.size() > best_strong_prefix.size()) {
                best_strong_prefix = key;
            }
        }
        if (!best_strong_prefix.empty()) {
            return best_strong_prefix;
        }

        for (const auto &key : regex_route_keys_) {
            const auto route_it = routes_.find(key);
            if (route_it == routes_.end() ||
                route_it->second.match_type != ProxyRoute::MatchType::regex) {
                continue;
            }
            if (std::regex_search(path, route_it->second.compiled_match)) {
                return key;
            }
        }

        auto result = url_trie_.find_prefix(path);
        if (!result || !result.is_registered)
            return "";
        const std::string key = path.substr(0, static_cast<size_t>(result.match_length));
        const auto route_it = routes_.find(key);
        if (route_it == routes_.end() ||
            (route_it->second.match_type != ProxyRoute::MatchType::prefix &&
             route_it->second.match_type != ProxyRoute::MatchType::prefix_strong)) {
            return "";
        }
        return key;
    }

    bool HttpProxy::is_proxy_url(const std::string & url) const
    {
        return !find_proxy_route(url).empty();
    }

    void HttpProxy::handle_websocket_upgrade_by_url(HttpRequest * req, HttpResponse * resp, const std::string & route_key)
    {
        if (!req || !resp || route_key.empty()) {
            if (resp)
                resp->process_error(ResponseCode::bad_gateway);
            return;
        }

        ProxyRoute route;
        {
            std::lock_guard<std::mutex> route_lock(route_mutex_);
            auto routeIt = routes_.find(route_key);
            if (routeIt == routes_.end()) {
                resp->process_error(ResponseCode::bad_gateway);
                return;
            }
            route = routeIt->second;
        }
        if (reject_method_if_not_allowed(req, resp, route.allowed_methods) ||
            reject_if_access_denied(req, resp, route.access_rules)) {
            return;
        }

        ProxyTarget target = select_target(route);
        auto *ctx = req->get_context();
        Connection *clientConn = ctx ? ctx->get_connection() : nullptr;
        if (!clientConn || !server_ || !server_->runtime()) {
            resp->process_error(ResponseCode::internal_server_error);
            ++stats_.failed_requests;
            return;
        }

        bool mapped = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            mapped = (cs_mapping_.find(clientConn) != cs_mapping_.end());
        }
        if (mapped) {
            ++stats_.ws_duplicate_upgrade_skipped;
            LOG_DEBUG_TAG("handle_websocket_upgrade_by_url",
                          "[Proxy][WS] skip duplicate upgrade route='{}' client={}",
                          route_key,
                          clientConn->get_remote_address().to_address_key());
            return;
        }

        auto task = handle_websocket_upgrade_async(req, resp, route_key, route, target, clientConn);
        task.resume();
        task.detach();
    }

    void HttpProxy::serve_proxy(HttpRequest * req, HttpResponse * resp)
    {
        auto task = serve_proxy_async(req, resp);
        task.resume();
        task.detach();
    }

    yuan::coroutine::Task<void> HttpProxy::serve_proxy_async(HttpRequest * req, HttpResponse * resp)
    {
        if (!req || !resp) {
            co_return;
        }

        ++stats_.total_requests;

        auto *ctx = req->get_context();
        Connection *clientConn = ctx ? ctx->get_connection() : nullptr;
        if (!clientConn) {
            resp->process_error(ResponseCode::internal_server_error);
            ++stats_.failed_requests;
            co_return;
        }

        const std::string route_key = find_proxy_route(req->get_raw_url());
        if (route_key.empty()) {
            resp->process_error(ResponseCode::not_found);
            co_return;
        }

        ProxyRoute route;
        {
            std::lock_guard<std::mutex> route_lock(route_mutex_);
            auto routeIt = routes_.find(route_key);
            if (routeIt == routes_.end()) {
                resp->process_error(ResponseCode::bad_gateway);
                ++stats_.failed_requests;
                co_return;
            }
            route = routeIt->second;
        }
        if (reject_method_if_not_allowed(req, resp, route.allowed_methods)) {
            co_return;
        }
        if (reject_if_access_denied(req, resp, route.access_rules)) {
            co_return;
        }

        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);
            auto csIt = cs_mapping_.find(clientConn);
            if (csIt != cs_mapping_.end() && csIt->second) {
                req->pack_and_send(csIt->second);
                csIt->second->flush();
                co_return;
            }

            auto reqIt = pending_requests_.find(clientConn);
            if (reqIt != pending_requests_.end()) {
                if (reqIt->second >= config::proxy_max_pending) {
                    resp->process_error(ResponseCode::service_unavailable);
                    ++stats_.failed_requests;
                } else {
                    ++reqIt->second;
                }
                co_return;
            }
        }

        co_await handle_proxy_async(req, resp, route_key, route, clientConn);
    }

    yuan::coroutine::Task<void> HttpProxy::handle_proxy_async(HttpRequest *req, HttpResponse *resp,
                                                              const std::string &route_key,
                                                              const ProxyRoute &route,
                                                              Connection *clientConn)
    {
        if (!server_ || !server_->runtime() || !clientConn || !req || !resp) {
            co_return;
        }

        const std::string original_url = req->get_raw_url();
        const std::string request_method = upper_ascii(req->get_raw_method());
        std::string cache_key;
        std::string cache_status;
        const bool cache_bypass = request_has_any_header(req, route.cache_bypass_headers);
        const bool cache_no_store = cache_bypass || request_has_any_header(req, route.cache_no_cache_headers);
        if (route.cache_enabled &&
            route.cache_ttl_ms > 0 &&
            route.cache_max_response_bytes > 0 &&
            route.cache_methods.find(request_method) != route.cache_methods.end()) {
            const std::string lookup_key = make_proxy_cache_key(req, route, route_key, original_url);
            std::string cached_payload;
            if (!cache_bypass) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                auto cache_it = response_cache_.find(lookup_key);
                if (cache_it != response_cache_.end()) {
                    const uint64_t now = now_ms_steady();
                    if (cache_it->second.expires_at_ms == 0 || cache_it->second.expires_at_ms > now) {
                        cached_payload = cache_it->second.payload;
                    } else {
                        response_cache_.erase(cache_it);
                    }
                }
            }
            if (!cached_payload.empty()) {
                clientConn->write_raw_and_flush(set_proxy_cache_header(cached_payload, "HIT"));
                co_return;
            }
            cache_status = cache_bypass ? "BYPASS" : "MISS";
            if (!cache_no_store) {
                cache_key = lookup_key;
            }
        }

        const int max_attempts = route.max_retries > 0 ? (route.max_retries + 1) : 1;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const ProxyTarget target = select_target(route);
            req->set_raw_url(original_url);
            build_forward_request(req, route, target);

            auto pool = get_or_create_pool(target, route);
            if (pool) {
                Connection *pooledConn = pool->acquire(this, server_);
                if (pooledConn) {
                    ++stats_.pool_hits;
                    LOG_DEBUG_TAG("handle_proxy_async",
                                  "[Proxy] pool hit route='{}' target={}:{} active={} total={} attempt={}/{}",
                                  route_key,
                                  target.host,
                                  target.port,
                                  pool->active_count(),
                                  pool->total_count(),
                                  attempt + 1,
                                  max_attempts);
                    map_connections(clientConn->shared_from_this(), pooledConn->shared_from_this(), route_key, cache_key, cache_status);
                    req->pack_and_send(pooledConn);
                    pooledConn->flush();
                    co_return;
                }
            }

            ++stats_.pool_misses;
            LOG_DEBUG_TAG("handle_proxy_async",
                          "[Proxy] pool miss route='{}' target={}:{} timeout_ms={} attempt={}/{}",
                          route_key,
                          target.host,
                          target.port,
                          route.connect_timeout_ms,
                          attempt + 1,
                          max_attempts);

            const uint64_t dial_start_ms = base::time::steady_now_ms();

            auto connect_result = co_await yuan::coroutine::async_connect(
                static_cast<yuan::coroutine::RuntimeView>(server_->runtime()->runtime_view()),
                target.host,
                target.port,
                route.connect_timeout_ms > 0 ? static_cast<uint32_t>(route.connect_timeout_ms) : 0U);

            const uint64_t dial_elapsed_ms = base::time::steady_now_ms() - dial_start_ms;

            if (connect_result.result != yuan::coroutine::ConnectResult::success || !connect_result.connection) {
                mark_target_failure(target, route);
                LOG_WARN_TAG("handle_proxy_async",
                             "[Proxy] upstream dial failed route='{}' target={}:{} result={} elapsed_ms={} timeout_ms={} attempt={}/{}",
                             route_key,
                             target.host,
                             target.port,
                             connect_result_text(connect_result.result),
                             dial_elapsed_ms,
                             route.connect_timeout_ms,
                             attempt + 1,
                             max_attempts);
                if (attempt + 1 < max_attempts) {
                    continue;
                }
                resp->process_error(connect_result.result == yuan::coroutine::ConnectResult::timed_out
                                        ? ResponseCode::gateway_timeout
                                        : ResponseCode::bad_gateway);
                clientConn->flush();
                ++stats_.failed_requests;
                co_return;
            }

            {
                std::lock_guard<std::mutex> lock(mapping_mutex_);
                if (cs_mapping_.find(clientConn) != cs_mapping_.end()) {
                    ++stats_.ws_stale_upgrade_skipped;
                    LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                                  "[Proxy][WS] skip stale upgrade after connect route='{}' target={}:{}",
                                  route_key,
                                  target.host,
                                  target.port);
                    co_return;
                }
            }

            LOG_DEBUG_TAG("handle_proxy_async",
                          "[Proxy] upstream dial success route='{}' target={}:{} elapsed_ms={} attempt={}/{}",
                          route_key,
                          target.host,
                          target.port,
                          dial_elapsed_ms,
                          attempt + 1,
                          max_attempts);

            auto remote_owner = connect_result.connection;
            Connection *remoteConn = &*remote_owner;
            server_->runtime()->register_connection(remote_owner, make_non_owning_handler(this));

            ++stats_.active_connections;
            map_connections(clientConn->shared_from_this(), remote_owner, route_key, cache_key, cache_status);
            req->pack_and_send(remoteConn);
            yuan::net::AsyncConnectionContext remote_ctx(remote_owner, server_->runtime()->runtime_view());
            auto write_result = co_await remote_ctx.flush_async(
                route.write_timeout_ms > 0 ? static_cast<uint32_t>(route.write_timeout_ms) : 0U);
            if (write_result.status != yuan::coroutine::IoStatus::success) {
                mark_target_failure(target, route);
                LOG_WARN_TAG("handle_proxy_async",
                             "[Proxy] upstream request write failed route='{}' target={}:{} status={} attempt={}/{}",
                             route_key,
                             target.host,
                             target.port,
                             static_cast<int>(write_result.status),
                             attempt + 1,
                             max_attempts);
                ++stats_.failed_requests;
                if (attempt + 1 < max_attempts) {
                    if (remoteConn) {
                        remoteConn->close();
                    }
                    (void)unmap_and_close_peer(clientConn, true);
                    continue;
                }
                co_return;
            }
            mark_target_success(target);
            co_return;
        }
        co_return;
    }

    void HttpProxy::on_client_close(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        auto *conn_ptr = &*conn;
        (void)unmap_and_close_peer(conn_ptr, true);
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        pending_requests_.erase(conn_ptr);
    }

    bool HttpProxy::has_client_mapping(Connection *conn) const
    {
        if (!conn) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        return cs_mapping_.find(conn) != cs_mapping_.end();
    }

    HttpProxyStats HttpProxy::snapshot_stats() const
    {
        HttpProxyStats out;
        out.total_requests = stats_.total_requests.load(std::memory_order_relaxed);
        out.active_connections = stats_.active_connections.load(std::memory_order_relaxed);
        out.failed_requests = stats_.failed_requests.load(std::memory_order_relaxed);
        out.pool_hits = stats_.pool_hits.load(std::memory_order_relaxed);
        out.pool_misses = stats_.pool_misses.load(std::memory_order_relaxed);
        out.ws_duplicate_upgrade_skipped = stats_.ws_duplicate_upgrade_skipped.load(std::memory_order_relaxed);
        out.ws_stale_upgrade_skipped = stats_.ws_stale_upgrade_skipped.load(std::memory_order_relaxed);
        out.unmapped_close_events = stats_.unmapped_close_events.load(std::memory_order_relaxed);
        return out;
    }

    std::vector<TargetHealthSnapshot> HttpProxy::snapshot_target_health() const
    {
        std::vector<TargetHealthSnapshot> out;
        const uint64_t now = now_ms_steady();
        std::lock_guard<std::mutex> lock(health_mutex_);
        out.reserve(target_health_.size());
        for (const auto &kv : target_health_) {
            TargetHealthSnapshot item;
            item.target_key = kv.first;
            item.consecutive_failures = kv.second.consecutive_failures;
            item.unhealthy_until_ms = kv.second.unhealthy_until_ms;
            item.healthy = (item.unhealthy_until_ms == 0 || now >= item.unhealthy_until_ms);
            out.push_back(std::move(item));
        }
        return out;
    }

    ProxyTarget HttpProxy::select_target(const ProxyRoute & route)
    {
        if (route.targets.empty())
            return ProxyTarget{};

        std::vector<ProxyTarget> healthy;
        healthy.reserve(route.targets.size());
        for (const auto &target : route.targets) {
            if (is_target_available(target)) {
                healthy.push_back(target);
            }
        }

        const auto &candidates = healthy.empty() ? route.targets : healthy;

        switch (route.balance) {
        case ProxyRoute::BalanceStrategy::round_robin: {
            std::lock_guard<std::mutex> lock(rr_mutex_);
            auto &idx = rr_indices_[route.match_pattern];
            size_t i = idx % candidates.size();
            idx++;
            return candidates[i];
        }
        case ProxyRoute::BalanceStrategy::random: {
            std::lock_guard<std::mutex> lock(rr_mutex_);
            std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
            return candidates[dist(rng_)];
        }
        case ProxyRoute::BalanceStrategy::least_connections:
            return select_least_connections(candidates);
        case ProxyRoute::BalanceStrategy::weighted_round_robin:
            return select_weighted_random(candidates);
        default:
            return candidates[0];
        }
    }

    ProxyTarget HttpProxy::select_weighted_random(const std::vector<ProxyTarget> & targets)
    {
        int total_weight = 0;
        for (const auto &t : targets)
            total_weight += (std::max)(1, t.weight);

        std::uniform_int_distribution<int> dist(1, total_weight);
        int rand_val = dist(rng_);

        int cumulative = 0;
        for (const auto &t : targets) {
            cumulative += (std::max)(1, t.weight);
            if (rand_val <= cumulative)
                return t;
        }
        return targets.back();
    }

    ProxyTarget HttpProxy::select_least_connections(const ProxyRoute & route)
    {
        return select_least_connections(route.targets);
    }

    ProxyTarget HttpProxy::select_least_connections(const std::vector<ProxyTarget> &targets)
    {
        if (targets.empty()) {
            return ProxyTarget{};
        }

        ProxyTarget best = targets[0];
        size_t min_active = SIZE_MAX;

        for (const auto &target : targets) {
            std::string pool_id = target.host + ":" + std::to_string(target.port);

            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                size_t active = it->second->active_count();
                if (active < min_active) {
                    min_active = active;
                    best = target;
                }
            } else {
                best = target;
                min_active = 0;
                break;
            }
        }

        return best;
    }

    void HttpProxy::build_forward_request(HttpRequest * orig_req, const ProxyRoute & route,
                                            const ProxyTarget & target, bool is_websocket)
    {
        std::string original_host;
        if (const auto *host = orig_req->get_header("host")) {
            original_host = *host;
        }

        std::string remote_addr;
        if (orig_req->get_context() && orig_req->get_context()->get_connection()) {
            const auto &addr = orig_req->get_context()->get_connection()->get_remote_address();
            remote_addr = addr.to_address_key();
        }

        std::string proxy_add_x_forwarded_for;
        if (auto *xff = orig_req->get_header("x-forwarded-for")) {
            proxy_add_x_forwarded_for = *xff;
            if (!remote_addr.empty()) {
                proxy_add_x_forwarded_for += ", ";
                proxy_add_x_forwarded_for += remote_addr;
            }
        } else {
            proxy_add_x_forwarded_for = remote_addr;
        }

        if (!route.preserve_host || original_host.empty()) {
            orig_req->add_header("Host", target.host + ":" + std::to_string(target.port));
        }

        if (!proxy_add_x_forwarded_for.empty()) {
            orig_req->add_header("X-Forwarded-For", proxy_add_x_forwarded_for);
        }

        if (!remote_addr.empty()) {
            orig_req->add_header("X-Real-IP", remote_addr);
        }

        orig_req->add_header("X-Forwarded-Proto", "http");

        for (const auto &header : route.request_headers) {
            if (header.second.empty()) {
                orig_req->remove_header(header.first);
                continue;
            }
            orig_req->add_header(header.first,
                                 expand_proxy_header_value(orig_req,
                                                           header.second,
                                                           original_host,
                                                           remote_addr,
                                                           proxy_add_x_forwarded_for));
        }

        for (const auto &header : route.hide_request_headers) {
            orig_req->remove_header(header);
        }

        if (!is_websocket) {
            orig_req->remove_header("connection");
            orig_req->remove_header("upgrade");
        }
        orig_req->remove_header("keep-alive");
        orig_req->remove_header("transfer-encoding");
        orig_req->remove_header("te");
        orig_req->remove_header("trailers");

        if (route.strip_prefix) {
            auto path = std::string(orig_req->get_path());
            auto query = std::string(orig_req->get_query_string());
            if (path.size() >= route.match_pattern.size()
                && path.compare(0, route.match_pattern.size(), route.match_pattern) == 0) {
                path.erase(0, route.match_pattern.size());
            }
            if (!route.rewrite_prefix.empty()) {
                if (!path.empty() && path.front() != '/') {
                    path.insert(path.begin(), '/');
                }
                path = route.rewrite_prefix + path;
            }
            if (path.empty()) {
                path = "/";
            }
            std::string new_url = path;
            if (!query.empty()) {
                new_url += "?";
                new_url += query;
            }
            orig_req->set_raw_url(std::move(new_url));
        }
    }

    std::string HttpProxy::target_key(const ProxyTarget &target) const
    {
        return target.host + ":" + std::to_string(target.port);
    }

    bool HttpProxy::is_target_available(const ProxyTarget &target) const
    {
        const uint64_t now = now_ms_steady();
        std::lock_guard<std::mutex> lock(health_mutex_);
        const auto it = target_health_.find(target_key(target));
        if (it == target_health_.end()) {
            return true;
        }
        return it->second.unhealthy_until_ms == 0 || now >= it->second.unhealthy_until_ms;
    }

    void HttpProxy::mark_target_failure(const ProxyTarget &target, const ProxyRoute &route)
    {
        if (route.failure_threshold <= 0 || route.unhealthy_cooldown_ms <= 0) {
            return;
        }
        const uint64_t now = now_ms_steady();
        const std::string key = target_key(target);
        std::lock_guard<std::mutex> lock(health_mutex_);
        auto &state = target_health_[key];
        ++state.consecutive_failures;
        if (state.consecutive_failures >= route.failure_threshold) {
            state.unhealthy_until_ms = now + static_cast<uint64_t>(route.unhealthy_cooldown_ms);
            LOG_WARN_TAG("mark_target_failure",
                         "[Proxy] mark target unhealthy {} failures={} cooldown_ms={}",
                         key,
                         state.consecutive_failures,
                         route.unhealthy_cooldown_ms);
        }
    }

    void HttpProxy::mark_target_success(const ProxyTarget &target)
    {
        const std::string key = target_key(target);
        std::lock_guard<std::mutex> lock(health_mutex_);
        auto it = target_health_.find(key);
        if (it == target_health_.end()) {
            return;
        }
        it->second.consecutive_failures = 0;
        it->second.unhealthy_until_ms = 0;
    }

    yuan::coroutine::Task<void> HttpProxy::handle_websocket_upgrade_async(HttpRequest *req,
                                                                           HttpResponse *resp,
                                                                           const std::string &route_key,
                                                                           const ProxyRoute &route,
                                                                           const ProxyTarget &target,
                                                                           Connection *clientConn)
    {
        if (!req || !resp || !clientConn || !server_ || !server_->runtime()) {
            co_return;
        }

        LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                      "[Proxy][WS] dialing upstream route='{}' target={}:{} timeout_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      route.connect_timeout_ms);

        const uint64_t dial_start_ms = base::time::steady_now_ms();

        auto connect_result = co_await yuan::coroutine::async_connect(
            static_cast<yuan::coroutine::RuntimeView>(server_->runtime()->runtime_view()),
            target.host,
            target.port,
            route.connect_timeout_ms > 0 ? static_cast<uint32_t>(route.connect_timeout_ms) : 0U);

        const uint64_t dial_elapsed_ms = base::time::steady_now_ms() - dial_start_ms;

        if (connect_result.result != yuan::coroutine::ConnectResult::success || !connect_result.connection) {
            LOG_WARN_TAG("handle_websocket_upgrade_async",
                         "[Proxy][WS] upstream dial failed route='{}' target={}:{} result={} elapsed_ms={} timeout_ms={}",
                         route_key,
                         target.host,
                         target.port,
                         connect_result_text(connect_result.result),
                         dial_elapsed_ms,
                         route.connect_timeout_ms);
            resp->process_error(connect_result.result == yuan::coroutine::ConnectResult::timed_out
                                    ? ResponseCode::gateway_timeout
                                    : ResponseCode::bad_gateway);
            ++stats_.failed_requests;
            co_return;
        }

        LOG_DEBUG_TAG("handle_websocket_upgrade_async",
                      "[Proxy][WS] upstream dial success route='{}' target={}:{} elapsed_ms={}",
                      route_key,
                      target.host,
                      target.port,
                      dial_elapsed_ms);

        auto remote_owner = connect_result.connection;
        Connection *remoteConn = &*remote_owner;
        server_->runtime()->register_connection(remote_owner, make_non_owning_handler(this));

        ++stats_.active_connections;
        map_connections(clientConn->shared_from_this(), remote_owner, route_key);

        req->pack_and_send(remoteConn);
        remoteConn->flush();
        co_return;
    }

    void HttpProxy::map_connections(Connection * clientConn, Connection * serverConn, const std::string & routeKey)
    {
        map_connections(clientConn, serverConn, routeKey, {}, {});
    }

    void HttpProxy::map_connections(Connection *clientConn,
                                    Connection *serverConn,
                                    const std::string &routeKey,
                                    std::string cacheKey,
                                    std::string cacheStatus)
    {
        std::lock_guard<std::mutex> lock(mapping_mutex_);
        cs_mapping_[clientConn] = serverConn;

        ServerMapping sm;
        sm.client_conn = clientConn;
        sm.route_key = routeKey;
        sm.cache_key = std::move(cacheKey);
        sm.cache_status = std::move(cacheStatus);
        sm.cache_candidate = !sm.cache_key.empty();
        sc_mapping_[serverConn] = std::move(sm);
    }

    void HttpProxy::map_connections(const std::shared_ptr<Connection> &clientConn,
                                    const std::shared_ptr<Connection> &serverConn,
                                    const std::string &routeKey,
                                    std::string cacheKey,
                                    std::string cacheStatus)
    {
        if (!clientConn || !serverConn) {
            return;
        }
        map_connections(&*clientConn, &*serverConn, routeKey, std::move(cacheKey), std::move(cacheStatus));
    }

    bool HttpProxy::unmap_and_close_peer(Connection * conn, bool is_client)
    {
        Connection *peer_to_close = nullptr;
        bool unmapped = false;
        {
            std::lock_guard<std::mutex> lock(mapping_mutex_);

            if (is_client) {
                auto it = cs_mapping_.find(conn);
                if (it != cs_mapping_.end()) {
                    Connection *serverConn = it->second;
                    cs_mapping_.erase(it);
                    unmapped = true;

                    if (serverConn) {
                        auto sit = sc_mapping_.find(serverConn);
                        if (sit != sc_mapping_.end()) {
                            sc_mapping_.erase(sit);
                        }
                        peer_to_close = serverConn;
                    }
                }
            } else {
                auto it = sc_mapping_.find(conn);
                if (it != sc_mapping_.end()) {
                    Connection *clientConn = it->second.client_conn;
                    sc_mapping_.erase(it);
                    unmapped = true;

                    if (clientConn) {
                        auto cit = cs_mapping_.find(clientConn);
                        if (cit != cs_mapping_.end()) {
                            cs_mapping_.erase(cit);
                        }
                        peer_to_close = clientConn;
                    }
                }
            }
        }

        if (peer_to_close) {
            peer_to_close->close();
        }

        return unmapped;
    }

    void HttpProxy::forward_data(Connection * src, Connection * dst)
    {
        if (!src || !dst)
            return;

        const auto input = src->take_input_byte_buffer();
        if (input.empty())
            return;
        dst->write(input);
    }

    bool HttpProxy::handle_websocket_upgrade(HttpRequest * req, HttpResponse * resp,
                                                const ProxyRoute & route, const ProxyTarget & target)
    {
        auto *upgrade = req->get_header("upgrade");
        if (!upgrade)
            return false;

        std::string upgrade_lower = *upgrade;
        std::transform(upgrade_lower.begin(), upgrade_lower.end(), upgrade_lower.begin(), ::tolower);
        if (upgrade_lower != "websocket")
            return false;

        build_forward_request(req, route, target, true);

        return true;
    }

    void HttpProxy::remove_connection_from_pools(Connection * conn)
    {
        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pool_snapshot.reserve(pools_.size());
            for (auto &pair : pools_) {
                if (pair.second) {
                    pool_snapshot.push_back(pair.second);
                }
            }
        }

        for (auto &pool : pool_snapshot) {
            pool->remove(conn);
        }
    }

    void HttpProxy::cleanup_idle_connections()
    {
        std::vector<std::shared_ptr<TargetConnectionPool> > pool_snapshot;
        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            pool_snapshot.reserve(pools_.size());
            for (auto &pair : pools_) {
                if (pair.second)
                    pool_snapshot.push_back(pair.second);
            }
        }

        size_t total_cleaned = 0;
        for (auto &pool : pool_snapshot) {
            if (pool) {
                total_cleaned += pool->cleanup_idle();
            }
        }

        if (total_cleaned > 0) {
            LOG_INFO_TAG("cleanup_idle_connections", "[Proxy] cleaned up {} idle connections from pool", total_cleaned);
        }
    }

    std::shared_ptr<TargetConnectionPool> HttpProxy::get_or_create_pool(const ProxyTarget & target,
                                                                        const ProxyRoute & route)
    {
        std::string pool_id = target.host + ":" + std::to_string(target.port);

        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                return it->second;
            }
        }

        auto pool = std::make_shared<TargetConnectionPool>(
            target,
            route.max_pool_size_per_target,
            std::chrono::seconds(route.idle_timeout_seconds));

        {
            std::lock_guard<std::mutex> lock(pools_mutex_);
            auto it = pools_.find(pool_id);
            if (it != pools_.end() && it->second) {
                return it->second;
            }
            pools_[pool_id] = pool;
        }

        return pool;
    }

    std::unique_ptr<HttpProxyHandler> create_http_proxy_handler(HttpServer &server)
    {
        return std::make_unique<HttpProxy>(&server);
    }

    } // namespace yuan::net::http
