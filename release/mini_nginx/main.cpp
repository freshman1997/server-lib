#include "application.h"
#include "bootstrap.h"
#include "http/http_service.h"
#include "middleware.h"
#include "nlohmann/json.hpp"
#include "ops/option.h"
#include "proxy.h"
#include "request.h"
#include "response.h"
#include "context.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{
    std::atomic_bool g_running{ true };
    std::atomic_bool g_reload_requested{ false };

    void terminate_handler(int)
    {
        g_running.store(false, std::memory_order_relaxed);
    }

    void reload_handler(int)
    {
        g_reload_requested.store(true, std::memory_order_relaxed);
    }

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

    std::string read_env_string(const char *name, const std::string &default_value = {})
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

    void print_usage(const char *program)
    {
        std::cout << "mini_nginx usage:\n"
                  << "  " << program << " [config.json]\n\n"
                  << "env overrides:\n"
                  << "  YUAN_MINI_NGINX_PORT\n"
                  << "  YUAN_MINI_NGINX_SERVER_NAME\n"
                  << "  YUAN_MINI_NGINX_WORKERS\n"
                  << "  YUAN_MINI_NGINX_USE_IOCP\n"
                  << "  YUAN_MINI_NGINX_ACCESS_LOG\n"
                  << "  YUAN_MINI_NGINX_ACCESS_LOG_PATH\n";
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
        if (root.empty() || root.front() != '/') {
            error = "route.root must start with '/'";
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

    struct RedirectRule
    {
        std::string from;
        std::string to;
        int code = 302;
        bool prefix = false;
        bool preserve_path = false;
    };

    struct MiniNginxConfig
    {
        MiniNginxConfig()
        {
            server_config.enable_ssl = false;
        }

        yuan::net::http::HttpServerConfig server_config;
        int listen_port = 8080;
        bool access_log_enabled = true;
        bool access_log_json = true;
        std::string access_log_path = "release/mini_nginx/access.log";
        int reload_check_interval_ms = 1000;
        bool rate_limit_enabled = false;
        int rate_limit_rps = 100;
        int rate_limit_burst = 50;
        int max_connections = 0;
        int max_connections_per_ip = 0;
        int max_inflight_requests_per_ip = 0;
        int max_concurrent_requests_per_ip = 0;
        int worker_processes = 1;
        bool expose_stats = true;
        bool health_enabled = true;
        bool health_json = false;
        std::string health_path = "/healthz";
        std::vector<std::pair<std::string, std::string>> response_headers;
        std::vector<RedirectRule> redirects;
        std::unordered_set<std::string> allowed_methods;
        std::vector<yuan::net::http::StaticMount> static_mounts;
        std::vector<nlohmann::json> routes;
    };

    bool parse_header_map(const nlohmann::json &obj,
                          std::vector<std::pair<std::string, std::string>> &headers)
    {
        if (!obj.is_object()) {
            std::cerr << "response headers must be an object\n";
            return false;
        }

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().is_string()) {
                std::cerr << "response header values must be strings\n";
                return false;
            }
            const auto name = it.key();
            if (name.empty() || name.find_first_of("\r\n:") != std::string::npos) {
                std::cerr << "response header name is invalid\n";
                return false;
            }
            headers.emplace_back(name, it.value().get<std::string>());
        }
        return true;
    }

    bool parse_edge_features(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        cfg.response_headers.clear();
        cfg.redirects.clear();
        cfg.allowed_methods.clear();

        if (json.contains("health")) {
            if (!json["health"].is_object()) {
                std::cerr << "config 'health' must be an object\n";
                return false;
            }
            const auto &health = json["health"];
            cfg.health_enabled = json_bool(health, "enabled", cfg.health_enabled);
            cfg.health_json = json_bool(health, "json", cfg.health_json);
            if (health.contains("path") && health["path"].is_string()) {
                cfg.health_path = health["path"].get<std::string>();
            }
            if (cfg.health_path.empty() || cfg.health_path.front() != '/') {
                std::cerr << "health.path must start with '/'\n";
                return false;
            }
        }

        if (json.contains("response_headers") &&
            !parse_header_map(json["response_headers"], cfg.response_headers)) {
            return false;
        }
        if (json.contains("headers")) {
            if (!json["headers"].is_object()) {
                std::cerr << "config 'headers' must be an object\n";
                return false;
            }
            if (json["headers"].contains("add")) {
                if (!parse_header_map(json["headers"]["add"], cfg.response_headers)) {
                    return false;
                }
            } else if (!parse_header_map(json["headers"], cfg.response_headers)) {
                return false;
            }
        }

        auto parse_allowed_methods = [&](const nlohmann::json &arr) {
            if (!arr.is_array()) {
                std::cerr << "allowed_methods must be an array\n";
                return false;
            }
            for (const auto &item : arr) {
                if (!item.is_string()) {
                    std::cerr << "allowed_methods values must be strings\n";
                    return false;
                }
                const auto method = to_upper_ascii(item.get<std::string>());
                if (method.empty()) {
                    std::cerr << "allowed_methods values must not be empty\n";
                    return false;
                }
                cfg.allowed_methods.insert(method);
            }
            return true;
        };

        if (json.contains("allowed_methods") && !parse_allowed_methods(json["allowed_methods"])) {
            return false;
        }
        if (json.contains("server") && json["server"].is_object() &&
            json["server"].contains("allowed_methods") &&
            !parse_allowed_methods(json["server"]["allowed_methods"])) {
            return false;
        }

        if (json.contains("redirects")) {
            if (!json["redirects"].is_array()) {
                std::cerr << "config 'redirects' must be an array\n";
                return false;
            }
            for (const auto &item : json["redirects"]) {
                if (!item.is_object()) {
                    std::cerr << "redirect item must be object\n";
                    return false;
                }
                if (!item.contains("from") || !item["from"].is_string() ||
                    !item.contains("to") || !item["to"].is_string()) {
                    std::cerr << "redirect.from and redirect.to are required strings\n";
                    return false;
                }
                RedirectRule rule;
                rule.from = item["from"].get<std::string>();
                rule.to = item["to"].get<std::string>();
                rule.code = json_int(item, "code", rule.code);
                rule.prefix = json_bool(item, "prefix", rule.prefix);
                rule.preserve_path = json_bool(item, "preserve_path", rule.preserve_path);
                if (rule.from.empty() || rule.from.front() != '/') {
                    std::cerr << "redirect.from must start with '/'\n";
                    return false;
                }
                if (rule.to.empty() || rule.to.find_first_of("\r\n") != std::string::npos) {
                    std::cerr << "redirect.to is invalid\n";
                    return false;
                }
                if (rule.code != 301 && rule.code != 302 && rule.code != 303) {
                    std::cerr << "redirect.code must be 301, 302, or 303\n";
                    return false;
                }
                cfg.redirects.push_back(std::move(rule));
            }
        }

        return true;
    }

    bool parse_static_mounts(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        if (!json.contains("static")) {
            return true;
        }
        if (!json["static"].is_array()) {
            std::cerr << "config 'static' must be an array\n";
            return false;
        }

        cfg.static_mounts.clear();
        for (const auto &item : json["static"]) {
            if (!item.is_object()) {
                std::cerr << "static item must be object\n";
                return false;
            }
            if (!item.contains("location") || !item["location"].is_string()) {
                std::cerr << "static.location(string) is required\n";
                return false;
            }
            if (!item.contains("root") || !item["root"].is_string()) {
                std::cerr << "static.root(string) is required\n";
                return false;
            }

            yuan::net::http::StaticMount mount;
            mount.prefix = item["location"].get<std::string>();
            mount.root = item["root"].get<std::string>();
            if (mount.prefix.empty() || mount.prefix.front() != '/') {
                std::cerr << "static.location must start with '/'\n";
                return false;
            }
            if (mount.root.empty()) {
                std::cerr << "static.root must not be empty\n";
                return false;
            }

            if (item.contains("auto_index") && item["auto_index"].is_boolean()) {
                mount.options.auto_index = item["auto_index"].get<bool>();
            }

            if (item.contains("enable_range") && item["enable_range"].is_boolean()) {
                mount.options.enable_range = item["enable_range"].get<bool>();
            }
            if (item.contains("index") && item["index"].is_array()) {
                mount.options.index_files.clear();
                for (const auto &index_name : item["index"]) {
                    if (index_name.is_string()) {
                        mount.options.index_files.push_back(index_name.get<std::string>());
                    }
                }
                if (mount.options.index_files.empty()) {
                    mount.options.index_files = { "index.html", "index.htm" };
                }
            }

            if (item.contains("try_files") && item["try_files"].is_array()) {
                mount.options.try_files.clear();
                for (const auto &tf : item["try_files"]) {
                    if (tf.is_string()) {
                        mount.options.try_files.push_back(tf.get<std::string>());
                    }
                }
            }

            if (item.contains("error_page") && item["error_page"].is_object()) {
                for (auto it = item["error_page"].begin(); it != item["error_page"].end(); ++it) {
                    if (!it.value().is_string()) {
                        continue;
                    }
                    try {
                        const int code = std::stoi(it.key());
                        mount.options.error_pages[code] = it.value().get<std::string>();
                    } catch (...) {
                    }
                }
            }

            cfg.static_mounts.push_back(std::move(mount));
        }
        return true;
    }

    void parse_listen_options(const nlohmann::json &server, MiniNginxConfig &cfg)
    {
        auto apply = [&](const nlohmann::json &options) {
            cfg.server_config.listen_options.reuse_addr =
                json_bool(options, "reuse_addr", cfg.server_config.listen_options.reuse_addr);
            cfg.server_config.listen_options.reuse_port =
                json_bool(options, "reuse_port", cfg.server_config.listen_options.reuse_port);
            cfg.server_config.listen_options.exclusive_addr =
                json_bool(options, "exclusive_addr", cfg.server_config.listen_options.exclusive_addr);
            cfg.server_config.listen_options.non_block =
                json_bool(options, "non_block", cfg.server_config.listen_options.non_block);
            cfg.server_config.listen_options.use_iocp =
                json_bool(options, "use_iocp", cfg.server_config.listen_options.use_iocp);
            cfg.server_config.listen_options.backlog =
                json_int(options, "backlog", cfg.server_config.listen_options.backlog);
            if (options.contains("iocp_worker_count") && options["iocp_worker_count"].is_number_unsigned()) {
                cfg.server_config.listen_options.iocp_worker_count =
                    options["iocp_worker_count"].get<std::size_t>();
            }
        };

        apply(server);
        if (server.contains("listen_options") && server["listen_options"].is_object()) {
            apply(server["listen_options"]);
        }
        if (cfg.server_config.listen_options.backlog < 1) {
            cfg.server_config.listen_options.backlog = 128;
        }
        if (cfg.server_config.listen_options.iocp_worker_count == 0) {
            cfg.server_config.listen_options.iocp_worker_count = 1;
        }
    }

    bool parse_server_config(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        if (json.contains("server")) {
            if (!json["server"].is_object()) {
                std::cerr << "config 'server' must be an object\n";
                return false;
            }

            const auto &server = json["server"];
            if (server.contains("listen") && server["listen"].is_number_integer()) {
                cfg.listen_port = server["listen"].get<int>();
            }
            if (server.contains("server_name") && server["server_name"].is_string()) {
                cfg.server_config.server_name = server["server_name"].get<std::string>();
            }
            if (server.contains("thread_pool_size") && server["thread_pool_size"].is_number_integer()) {
                cfg.server_config.thread_pool_size = server["thread_pool_size"].get<int>();
            }
            if (server.contains("enable_keep_alive") && server["enable_keep_alive"].is_boolean()) {
                cfg.server_config.enable_keep_alive = server["enable_keep_alive"].get<bool>();
            }
            if (server.contains("enable_ssl") && server["enable_ssl"].is_boolean()) {
                cfg.server_config.enable_ssl = server["enable_ssl"].get<bool>();
            }
            if (server.contains("ssl_certificate") && server["ssl_certificate"].is_string()) {
                cfg.server_config.ssl_certificate = server["ssl_certificate"].get<std::string>();
            }
            if (server.contains("ssl_certificate_key") && server["ssl_certificate_key"].is_string()) {
                cfg.server_config.ssl_certificate_key = server["ssl_certificate_key"].get<std::string>();
            }
            if (server.contains("enable_cors") && server["enable_cors"].is_boolean()) {
                cfg.server_config.enable_cors = server["enable_cors"].get<bool>();
            }
            if (server.contains("max_body_size") && server["max_body_size"].is_number_unsigned()) {
                cfg.server_config.max_body_size = server["max_body_size"].get<size_t>();
            }
            if (server.contains("write_timeout_ms") && server["write_timeout_ms"].is_number_integer()) {
                cfg.server_config.write_timeout_ms = server["write_timeout_ms"].get<int>();
            }
            if (server.contains("enable_http2") && server["enable_http2"].is_boolean()) {
                cfg.server_config.enable_http2 = server["enable_http2"].get<bool>();
            }
            if (server.contains("max_connections") && server["max_connections"].is_number_integer()) {
                cfg.server_config.max_connections = server["max_connections"].get<int>();
            }
            if (server.contains("max_connections_per_ip") && server["max_connections_per_ip"].is_number_integer()) {
                cfg.server_config.max_connections_per_ip = server["max_connections_per_ip"].get<int>();
            }
            if (server.contains("max_inflight_requests_per_ip") && server["max_inflight_requests_per_ip"].is_number_integer()) {
                cfg.server_config.max_inflight_requests_per_ip = server["max_inflight_requests_per_ip"].get<int>();
            }
            if (server.contains("max_concurrent_requests_per_ip") && server["max_concurrent_requests_per_ip"].is_number_integer()) {
                cfg.server_config.max_inflight_requests_per_ip = server["max_concurrent_requests_per_ip"].get<int>();
            }
            if (server.contains("worker_processes") && server["worker_processes"].is_number_integer()) {
                cfg.worker_processes = server["worker_processes"].get<int>();
            }
            parse_listen_options(server, cfg);
        }

        if (json.contains("access_log") && json["access_log"].is_object()) {
            const auto &access_log = json["access_log"];
            if (access_log.contains("enabled") && access_log["enabled"].is_boolean()) {
                cfg.access_log_enabled = access_log["enabled"].get<bool>();
            }
            if (access_log.contains("json") && access_log["json"].is_boolean()) {
                cfg.access_log_json = access_log["json"].get<bool>();
            }
            if (access_log.contains("path") && access_log["path"].is_string()) {
                cfg.access_log_path = access_log["path"].get<std::string>();
            }
        }

        if (json.contains("reload_check_interval_ms") && json["reload_check_interval_ms"].is_number_integer()) {
            cfg.reload_check_interval_ms = json["reload_check_interval_ms"].get<int>();
        }

        if (json.contains("rate_limit") && json["rate_limit"].is_object()) {
            const auto &rate_limit = json["rate_limit"];
            if (rate_limit.contains("enabled") && rate_limit["enabled"].is_boolean()) {
                cfg.rate_limit_enabled = rate_limit["enabled"].get<bool>();
            }
            if (rate_limit.contains("requests_per_second") && rate_limit["requests_per_second"].is_number_integer()) {
                cfg.rate_limit_rps = rate_limit["requests_per_second"].get<int>();
            }
            if (rate_limit.contains("burst") && rate_limit["burst"].is_number_integer()) {
                cfg.rate_limit_burst = rate_limit["burst"].get<int>();
            }
        }

        return true;
    }

    bool build_routes_from_upstreams(const nlohmann::json &json,
                                     std::vector<nlohmann::json> &routes,
                                     bool require_routes)
    {
        if (!json.contains("routes")) {
            if (require_routes) {
                std::cerr << "config 'routes' is required\n";
                return false;
            }
            return true;
        }
        if (!json.contains("upstreams") || !json["upstreams"].is_object()) {
            std::cerr << "config with 'routes' must provide object 'upstreams'\n";
            return false;
        }
        if (!json["routes"].is_array()) {
            std::cerr << "config 'routes' must be an array\n";
            return false;
        }

        const auto &upstreams = json["upstreams"];
        for (const auto &route_item : json["routes"]) {
            if (!route_item.is_object()) {
                std::cerr << "route item must be object\n";
                return false;
            }
            if (!route_item.contains("path") || !route_item["path"].is_string()) {
                std::cerr << "route.path(string) is required\n";
                return false;
            }
            if (!route_item.contains("proxy_pass") || !route_item["proxy_pass"].is_string()) {
                std::cerr << "route.proxy_pass(string upstream name) is required\n";
                return false;
            }

            const std::string upstream_name = route_item["proxy_pass"].get<std::string>();
            if (!upstreams.contains(upstream_name) || !upstreams[upstream_name].is_object()) {
                std::cerr << "undefined upstream: " << upstream_name << '\n';
                return false;
            }

            const auto &upstream = upstreams[upstream_name];
            if (!upstream.contains("servers") || !upstream["servers"].is_array() || upstream["servers"].empty()) {
                std::cerr << "upstream '" << upstream_name << "' requires non-empty servers array\n";
                return false;
            }

            nlohmann::json normalized = nlohmann::json::object();
            normalized["root"] = route_item["path"].get<std::string>();
            normalized["target"] = nlohmann::json::array();

            for (const auto &server : upstream["servers"]) {
                if (!server.is_object() ||
                    !server.contains("host") || !server["host"].is_string() ||
                    !server.contains("port") || !server["port"].is_number_unsigned()) {
                    std::cerr << "upstream server requires host(string) and port(unsigned)\n";
                    return false;
                }
                const auto port = server["port"].get<uint64_t>();
                if (port == 0 || port > 65535) {
                    std::cerr << "upstream server port must be in range [1, 65535]\n";
                    return false;
                }

                nlohmann::json target = nlohmann::json::array();
                target.push_back(server["host"].get<std::string>());
                target.push_back(static_cast<uint16_t>(port));
                if (server.contains("weight") && server["weight"].is_number_integer()) {
                    target.push_back(server["weight"].get<int>());
                }
                normalized["target"].push_back(target);
            }

            auto copy_if_present = [&](const char *key) {
                if (route_item.contains(key)) {
                    normalized[key] = route_item[key];
                } else if (upstream.contains(key)) {
                    normalized[key] = upstream[key];
                }
            };

            copy_if_present("balance");
            copy_if_present("strip_prefix");
            copy_if_present("rewrite");
            copy_if_present("connect_timeout");
            copy_if_present("read_timeout");
            copy_if_present("write_timeout");
            copy_if_present("max_retries");
            copy_if_present("failure_threshold");
            copy_if_present("unhealthy_cooldown_ms");
            copy_if_present("pool_size");
            copy_if_present("idle_timeout");

            std::string error;
            if (!validate_route(normalized, error)) {
                std::cerr << "invalid normalized route: " << error << '\n';
                return false;
            }
            routes.push_back(std::move(normalized));
        }

        return true;
    }

    bool load_and_apply_config(const std::string &path, MiniNginxConfig &cfg)
    {
        std::ifstream input(path);
        if (!input.good()) {
            std::cerr << "failed to open config file: " << path << '\n';
            return false;
        }

        nlohmann::json json;
        try {
            input >> json;
        } catch (const std::exception &ex) {
            std::cerr << "failed to parse config json: " << ex.what() << '\n';
            return false;
        }

        if (!parse_server_config(json, cfg)) {
            return false;
        }
        if (!parse_edge_features(json, cfg)) {
            return false;
        }

        cfg.routes.clear();
        if (!parse_static_mounts(json, cfg)) {
            return false;
        }

        const bool require_routes = cfg.static_mounts.empty();
        if (!build_routes_from_upstreams(json, cfg.routes, require_routes)) {
            return false;
        }

        if (cfg.routes.empty() && cfg.static_mounts.empty()) {
            std::cerr << "at least one route or static mount is required\n";
            return false;
        }
        if (cfg.listen_port <= 0 || cfg.listen_port > 65535) {
            std::cerr << "server.listen must be in range [1, 65535]\n";
            return false;
        }

        return true;
    }

    void install_static_mounts(yuan::net::http::HttpServer &server, const MiniNginxConfig &cfg)
    {
        for (const auto &mount : cfg.static_mounts) {
            server.mount_static(mount.prefix, mount.root, mount.options);
            std::cout << "static mount: " << mount.prefix << " -> " << mount.root << '\n';
        }
    }

    void apply_env_overrides(yuan::net::http::HttpServerConfig &server_config, int &listen_port)
    {
        const std::string server_name = read_env_string("YUAN_MINI_NGINX_SERVER_NAME");
        if (!server_name.empty()) {
            server_config.server_name = server_name;
        }
        listen_port = read_env_int("YUAN_MINI_NGINX_PORT", listen_port);
    }

    void apply_env_overrides(MiniNginxConfig &cfg)
    {
        apply_env_overrides(cfg.server_config, cfg.listen_port);

        const std::string enabled = read_env_string("YUAN_MINI_NGINX_ACCESS_LOG");
        if (!enabled.empty()) {
            cfg.access_log_enabled = (enabled == "1" || enabled == "true" || enabled == "TRUE");
        }

        const std::string path = read_env_string("YUAN_MINI_NGINX_ACCESS_LOG_PATH");
        if (!path.empty()) {
            cfg.access_log_path = path;
        }

        cfg.worker_processes = read_env_int("YUAN_MINI_NGINX_WORKERS", cfg.worker_processes);
        if (cfg.worker_processes < 1) {
            cfg.worker_processes = 1;
        }

        const std::string use_iocp = read_env_string("YUAN_MINI_NGINX_USE_IOCP");
        if (!use_iocp.empty()) {
            cfg.server_config.listen_options.use_iocp =
                (use_iocp == "1" || use_iocp == "true" || use_iocp == "TRUE");
        }
    }

    void install_routes(yuan::net::http::HttpProxyHandler *proxy, const std::vector<nlohmann::json> &routes)
    {
        if (!proxy) {
            return;
        }

        for (const auto &route_json : routes) {
            yuan::net::http::ProxyRoute route;
            route.match_pattern = route_json["root"].get<std::string>();

            for (const auto &upstream : route_json["target"]) {
                yuan::net::http::ProxyTarget target;
                target.host = upstream[0].get<std::string>();
                target.port = upstream[1].get<uint16_t>();
                if (upstream.size() >= 3 && upstream[2].is_number_integer()) {
                    target.weight = upstream[2].get<int>();
                }
                route.targets.push_back(std::move(target));
            }

            if (route_json.contains("balance") && route_json["balance"].is_string()) {
                const std::string balance = route_json["balance"].get<std::string>();
                if (balance == "random") {
                    route.balance = yuan::net::http::ProxyRoute::BalanceStrategy::random;
                } else if (balance == "least_conn") {
                    route.balance = yuan::net::http::ProxyRoute::BalanceStrategy::least_connections;
                } else if (balance == "weighted_rr") {
                    route.balance = yuan::net::http::ProxyRoute::BalanceStrategy::weighted_round_robin;
                }
            }

            if (route_json.contains("strip_prefix") && route_json["strip_prefix"].is_boolean()) {
                route.strip_prefix = route_json["strip_prefix"].get<bool>();
            }
            if (route_json.contains("rewrite") && route_json["rewrite"].is_string()) {
                route.rewrite_prefix = route_json["rewrite"].get<std::string>();
            }
            if (route_json.contains("connect_timeout") && route_json["connect_timeout"].is_number_integer()) {
                route.connect_timeout_ms = route_json["connect_timeout"].get<int>();
            }
            if (route_json.contains("read_timeout") && route_json["read_timeout"].is_number_integer()) {
                route.read_timeout_ms = route_json["read_timeout"].get<int>();
            }
            if (route_json.contains("write_timeout") && route_json["write_timeout"].is_number_integer()) {
                route.write_timeout_ms = route_json["write_timeout"].get<int>();
            }
            if (route_json.contains("max_retries") && route_json["max_retries"].is_number_integer()) {
                route.max_retries = route_json["max_retries"].get<int>();
            }
            if (route_json.contains("failure_threshold") && route_json["failure_threshold"].is_number_integer()) {
                route.failure_threshold = route_json["failure_threshold"].get<int>();
            }
            if (route_json.contains("unhealthy_cooldown_ms") && route_json["unhealthy_cooldown_ms"].is_number_integer()) {
                route.unhealthy_cooldown_ms = route_json["unhealthy_cooldown_ms"].get<int>();
            }
            if (route_json.contains("pool_size") && route_json["pool_size"].is_number_unsigned()) {
                route.max_pool_size_per_target = route_json["pool_size"].get<size_t>();
            }
            if (route_json.contains("idle_timeout") && route_json["idle_timeout"].is_number_unsigned()) {
                route.idle_timeout_seconds = route_json["idle_timeout"].get<size_t>();
            }

            proxy->add_route(route);
        }
    }

    void install_protection_middlewares(yuan::server::HttpService &http_service, const MiniNginxConfig &cfg)
    {
        if (!cfg.rate_limit_enabled) {
            return;
        }

        const int rps = cfg.rate_limit_rps > 0 ? cfg.rate_limit_rps : 100;
        const int burst = cfg.rate_limit_burst >= 0 ? cfg.rate_limit_burst : 50;
        http_service.server().use(yuan::net::http::middlewares::rate_limit(rps, burst));
        std::cout << "rate limit enabled: rps=" << rps << " burst=" << burst << '\n';
    }

    void install_edge_middlewares(yuan::server::HttpService &http_service, const MiniNginxConfig &cfg)
    {
        if (cfg.allowed_methods.empty() && cfg.response_headers.empty() && cfg.redirects.empty()) {
            return;
        }

        auto allowed = cfg.allowed_methods;
        auto allow_header = join_methods(allowed);
        auto headers = cfg.response_headers;
        auto redirects = cfg.redirects;

        http_service.server().use(
            [allowed = std::move(allowed),
             allow_header = std::move(allow_header),
             headers = std::move(headers),
             redirects = std::move(redirects)](yuan::net::http::HttpRequest *req,
                                               yuan::net::http::HttpResponse *resp) {
                if (!req || !resp) {
                    return yuan::net::http::MiddlewareResult::next;
                }

                for (const auto &header : headers) {
                    resp->add_header(header.first, header.second);
                }

                if (!allowed.empty()) {
                    const auto method = to_upper_ascii(req->get_raw_method());
                    if (allowed.find(method) == allowed.end()) {
                        const std::string body = "405 Method Not Allowed\n";
                        resp->set_response_code(yuan::net::http::ResponseCode::method_not_allowed);
                        resp->add_header("Allow", allow_header);
                        resp->add_header("Content-Type", "text/plain; charset=utf-8");
                        resp->append_body(body);
                        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
                        resp->send();
                        return yuan::net::http::MiddlewareResult::stop;
                    }
                }

                const auto path_view = req->get_path();
                const std::string path(path_view.data(), path_view.size());
                for (const auto &rule : redirects) {
                    bool matched = false;
                    if (rule.prefix) {
                        matched = path.rfind(rule.from, 0) == 0;
                    } else {
                        matched = path == rule.from;
                    }
                    if (!matched) {
                        continue;
                    }

                    std::string target = rule.to;
                    if (rule.prefix && rule.preserve_path && path.size() > rule.from.size()) {
                        if (!target.empty() && target.back() == '/' && path[rule.from.size()] == '/') {
                            target.pop_back();
                        }
                        target.append(path.substr(rule.from.size()));
                    }
                    resp->redirect(target, static_cast<yuan::net::http::ResponseCode>(rule.code));
                    resp->add_header("Content-Length", "0");
                    resp->send();
                    return yuan::net::http::MiddlewareResult::stop;
                }

                return yuan::net::http::MiddlewareResult::next;
            },
            "mini_nginx_edge");
    }

    void install_edge_routes(yuan::net::http::HttpServer &server, const MiniNginxConfig &cfg)
    {
        if (!cfg.health_enabled) {
            return;
        }

        const auto path = cfg.health_path;
        const bool json = cfg.health_json;
        server.on(path, [json](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (json) {
                nlohmann::json body;
                body["status"] = "ok";
                body["service"] = "mini_nginx";
                resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
            } else {
                resp->set_response_code(yuan::net::http::ResponseCode::ok_);
                resp->add_header("Content-Type", "text/plain; charset=utf-8");
                resp->append_body("ok\n");
                resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
            }
            if (req->get_version() != yuan::net::http::HttpVersion::v_2_0) {
                resp->send();
            }
        });
    }

    std::string now_iso8601_local()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

    void install_access_log(yuan::server::HttpService &http_service, const MiniNginxConfig &cfg)
    {
        if (!cfg.access_log_enabled) {
            std::cout << "access log disabled\n";
            return;
        }

        auto stream = std::make_shared<std::ofstream>(cfg.access_log_path, std::ios::out | std::ios::app);
        if (!stream->good()) {
            std::error_code ec;
            const auto parent = std::filesystem::path(cfg.access_log_path).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
                stream = std::make_shared<std::ofstream>(cfg.access_log_path, std::ios::out | std::ios::app);
            }
        }
        if (!stream->good()) {
            std::cerr << "failed to open access log file: " << cfg.access_log_path << '\n';
            return;
        }
        auto lock = std::make_shared<std::mutex>();

        const bool json_format = cfg.access_log_json;
        http_service.server().set_access_log_hook([stream, lock, json_format](yuan::net::http::HttpRequest *req,
                                                                              yuan::net::http::HttpResponse *resp,
                                                                              uint64_t) {
            if (!req || !resp) {
                return;
            }

            const auto ts = now_iso8601_local();
            const std::string method = req->get_raw_method();
            const std::string url = req->get_raw_url();
            std::string ip = "-";
            if (req->get_context() && req->get_context()->get_connection()) {
                ip = req->get_context()->get_connection()->get_remote_address().to_address_key();
            }

            int status = static_cast<int>(resp->get_response_code());
            if (status <= 0) {
                status = 200;
            }
            std::string upstream = "-";
            if (const auto *host = req->get_header("host")) {
                upstream = *host;
            }

            uint64_t latency_ms = 0;
            if (const auto *ctx = req->get_context()) {
                latency_ms = ctx->request_elapsed_ms();
            }

            std::lock_guard<std::mutex> guard(*lock);
            if (json_format) {
                nlohmann::json line;
                line["ts"] = ts;
                line["ip"] = ip;
                line["method"] = method.empty() ? "-" : method;
                line["url"] = url.empty() ? "/" : url;
                line["status"] = status;
                line["upstream"] = upstream;
                line["latency_ms"] = latency_ms;
                (*stream) << line.dump() << '\n';
            } else {
                (*stream) << ts
                          << " ip=" << ip
                          << " method=" << (method.empty() ? "-" : method)
                          << " url=" << (url.empty() ? "/" : url)
                          << " status=" << status
                          << " upstream=" << upstream
                          << " latency_ms=" << latency_ms
                          << '\n';
            }
            stream->flush();
        });

        std::cout << "access log enabled: " << cfg.access_log_path << '\n';
    }

    std::shared_ptr<yuan::server::HttpService> create_http_service(const MiniNginxConfig &cfg)
    {
        auto service = std::make_shared<yuan::server::HttpService>(cfg.listen_port, cfg.server_config);
        install_protection_middlewares(*service, cfg);
        install_edge_middlewares(*service, cfg);
        install_access_log(*service, cfg);
        service->set_server_configurator([cfg](yuan::server::HttpService &http_service) {
            auto *proxy = http_service.server().ensure_proxy();
            if (!proxy) {
                std::cerr << "http proxy module is unavailable\n";
                return false;
            }

            install_edge_routes(http_service.server(), cfg);
            install_static_mounts(http_service.server(), cfg);
            install_routes(proxy, cfg.routes);
            return true;
        });
        return service;
    }

    bool reload_routes(yuan::net::http::HttpProxyHandler *proxy,
                       const std::string &config_path,
                       yuan::net::http::HttpServer &server,
                       MiniNginxConfig &active_cfg)
    {
        MiniNginxConfig next_cfg = active_cfg;
        if (!load_and_apply_config(config_path, next_cfg)) {
            std::cerr << "reload failed: config parse failed, keep old routes\n";
            return false;
        }

        apply_env_overrides(next_cfg);
        if (next_cfg.listen_port != active_cfg.listen_port) {
            std::cerr << "reload note: listen port change requires restart (current="
                      << active_cfg.listen_port << ", new=" << next_cfg.listen_port << ")\n";
        }
        if (next_cfg.static_mounts.size() != active_cfg.static_mounts.size()) {
            std::cerr << "reload note: static mount changes require restart\n";
        }

        proxy->clear_routes();
        install_routes(proxy, next_cfg.routes);
        server.update_runtime_limits(next_cfg.server_config.max_connections,
                                     next_cfg.server_config.max_connections_per_ip,
                                     next_cfg.server_config.max_inflight_requests_per_ip);
        active_cfg.routes = std::move(next_cfg.routes);
        active_cfg.reload_check_interval_ms = next_cfg.reload_check_interval_ms;
        active_cfg.server_config.max_connections = next_cfg.server_config.max_connections;
        active_cfg.server_config.max_connections_per_ip = next_cfg.server_config.max_connections_per_ip;
        active_cfg.server_config.max_inflight_requests_per_ip = next_cfg.server_config.max_inflight_requests_per_ip;

        std::cout << "reload success: routes=" << active_cfg.routes.size() << '\n';
        return true;
    }
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, terminate_handler);
    std::signal(SIGTERM, terminate_handler);
#ifndef _WIN32
    std::signal(SIGHUP, reload_handler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    if (argc >= 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_usage(argv[0]);
        return 0;
    }

    std::string config_path = "release/mini_nginx/mini_nginx.json";
    if (argc >= 2) {
        config_path = argv[1];
    }

    MiniNginxConfig cfg;
    if (!load_and_apply_config(config_path, cfg)) {
        return 1;
    }
    apply_env_overrides(cfg);

    yuan::app::RuntimeContext context;
    context.app_name = "mini-nginx";
    const int requested_worker_processes = cfg.worker_processes < 1 ? 1 : cfg.worker_processes;
    int effective_worker_processes = requested_worker_processes;
    if (requested_worker_processes > 1) {
#ifdef _WIN32
        std::cerr << "worker_processes>1 requires POSIX multi-process mode; falling back to one worker on Windows\n";
        effective_worker_processes = 1;
#else
        context.run_mode = yuan::app::RunMode::multi_process;
        context.worker_threads = static_cast<std::size_t>(requested_worker_processes);
        context.runtime_workers.worker_count = static_cast<std::size_t>(requested_worker_processes);
        context.runtime_workers.process_mode = yuan::app::WorkerProcessMode::process_per_worker;
#endif
    }

    yuan::app::Application application(context);
    yuan::server::HttpService *local_http_service = nullptr;

    yuan::app::ServiceDescriptor http_descriptor;
    http_descriptor.name = "http";
    http_descriptor.type_name = "yuan::server::HttpService";
    http_descriptor.contract_id = "server.http";
    http_descriptor.contract_version = 1;
    http_descriptor.placement.mode = effective_worker_processes > 1
        ? yuan::app::PlacementMode::all_workers
        : yuan::app::PlacementMode::singleton;
    http_descriptor.placement.instances = static_cast<std::size_t>(effective_worker_processes);
    http_descriptor.endpoints.push_back(yuan::app::ServiceEndpoint{
        "http",
        "0.0.0.0",
        cfg.listen_port,
        "tcp"
    });

    if (!application.add_service(http_descriptor, [&cfg, &local_http_service]() {
            auto service = create_http_service(cfg);
            local_http_service = service.get();
            return service;
        })) {
        std::cerr << "failed to register http service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start mini_nginx service\n";
        return 1;
    }

    auto *proxy = local_http_service ? local_http_service->server().ensure_proxy() : nullptr;
    if (!proxy && bootstrap.process_role() != yuan::app::ProcessRole::supervisor) {
        std::cerr << "http proxy module is unavailable\n";
        bootstrap.shutdown();
        return 1;
    }

    std::cout << "mini_nginx listening on 0.0.0.0:" << cfg.listen_port << " using config " << config_path << '\n';
    std::cout << "routes loaded: " << cfg.routes.size() << '\n';
    std::cout << "static mounts loaded: " << cfg.static_mounts.size() << '\n';
    std::cout << "worker processes: " << effective_worker_processes << '\n';
#ifdef _WIN32
    std::cout << "listen backend: " << (cfg.server_config.listen_options.use_iocp ? "iocp" : "default") << '\n';
#else
    std::cout << "listen backend: default\n";
#endif
    std::cout << "reload check interval: " << cfg.reload_check_interval_ms << " ms\n";

    std::error_code ec;
    auto last_config_write_time = std::filesystem::last_write_time(config_path, ec);
    if (ec) {
        last_config_write_time = std::filesystem::file_time_type::min();
    }

    auto last_reload_check = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        bootstrap.poll_workers();

        const auto now = std::chrono::steady_clock::now();
        if (cfg.reload_check_interval_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reload_check).count() >= cfg.reload_check_interval_ms) {
            last_reload_check = now;
            std::error_code check_ec;
            const auto current_write = std::filesystem::last_write_time(config_path, check_ec);
            if (!check_ec && current_write != last_config_write_time) {
                last_config_write_time = current_write;
                g_reload_requested.store(true, std::memory_order_relaxed);
            }
        }

        if (g_reload_requested.exchange(false, std::memory_order_relaxed)) {
            if (proxy && local_http_service) {
                (void)reload_routes(proxy, config_path, local_http_service->server(), cfg);
            } else {
                std::cerr << "reload note: multi-process route reload requires restart\n";
            }
        }

        if (bootstrap.process_role() == yuan::app::ProcessRole::supervisor &&
            (bootstrap.has_failed_workers() ||
             (!bootstrap.has_running_workers() && !bootstrap.has_recovering_workers()))) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    bootstrap.shutdown();
    return 0;
}
