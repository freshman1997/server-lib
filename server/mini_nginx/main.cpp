#include "application.h"
#include "bootstrap.h"
#include "http_service.h"
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

    void print_usage(const char *program)
    {
        std::cout << "mini_nginx usage:\n"
                  << "  " << program << " [config.json]\n\n"
                  << "env overrides:\n"
                  << "  YUAN_MINI_NGINX_PORT\n"
                  << "  YUAN_MINI_NGINX_SERVER_NAME\n"
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
        }
        return true;
    }

    struct MiniNginxConfig
    {
        yuan::net::http::HttpServerConfig server_config;
        int listen_port = 8080;
        bool access_log_enabled = true;
        bool access_log_json = true;
        std::string access_log_path = "server/mini_nginx/access.log";
        int reload_check_interval_ms = 1000;
        std::vector<yuan::net::http::StaticMount> static_mounts;
        std::vector<nlohmann::json> routes;
    };

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

            if (item.contains("auto_index") && item["auto_index"].is_boolean()) {
                mount.options.auto_index = item["auto_index"].get<bool>();
            }
            if (item.contains("enable_legacy_embedded_pages") && item["enable_legacy_embedded_pages"].is_boolean()) {
                mount.options.enable_legacy_embedded_pages = item["enable_legacy_embedded_pages"].get<bool>();
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

    bool parse_server_config(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        if (json.contains("server") && json["server"].is_object()) {
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
            if (server.contains("enable_cors") && server["enable_cors"].is_boolean()) {
                cfg.server_config.enable_cors = server["enable_cors"].get<bool>();
            }
            if (server.contains("max_body_size") && server["max_body_size"].is_number_unsigned()) {
                cfg.server_config.max_body_size = server["max_body_size"].get<size_t>();
            }
            if (server.contains("enable_http2") && server["enable_http2"].is_boolean()) {
                cfg.server_config.enable_http2 = server["enable_http2"].get<bool>();
            }
            return true;
        }

        if (json.contains("listen") && json["listen"].is_number_integer()) {
            cfg.listen_port = json["listen"].get<int>();
        }
        if (json.contains("server_name") && json["server_name"].is_string()) {
            cfg.server_config.server_name = json["server_name"].get<std::string>();
        }
        if (json.contains("thread_pool_size") && json["thread_pool_size"].is_number_integer()) {
            cfg.server_config.thread_pool_size = json["thread_pool_size"].get<int>();
        }
        if (json.contains("enable_keep_alive") && json["enable_keep_alive"].is_boolean()) {
            cfg.server_config.enable_keep_alive = json["enable_keep_alive"].get<bool>();
        }
        if (json.contains("enable_cors") && json["enable_cors"].is_boolean()) {
            cfg.server_config.enable_cors = json["enable_cors"].get<bool>();
        }
        if (json.contains("max_body_size") && json["max_body_size"].is_number_unsigned()) {
            cfg.server_config.max_body_size = json["max_body_size"].get<size_t>();
        }
        if (json.contains("enable_http2") && json["enable_http2"].is_boolean()) {
            cfg.server_config.enable_http2 = json["enable_http2"].get<bool>();
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

        return true;
    }

    bool build_routes_from_legacy_proxies(const nlohmann::json &json, std::vector<nlohmann::json> &routes)
    {
        if (!json.contains("proxies")) {
            return true;
        }
        if (!json["proxies"].is_array()) {
            std::cerr << "config 'proxies' must be an array\n";
            return false;
        }

        for (const auto &route : json["proxies"]) {
            std::string error;
            if (!validate_route(route, error)) {
                std::cerr << "invalid route in proxies: " << error << '\n';
                return false;
            }
            routes.push_back(route);
        }
        return true;
    }

    bool build_routes_from_upstreams(const nlohmann::json &json, std::vector<nlohmann::json> &routes)
    {
        if (!json.contains("routes")) {
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

                nlohmann::json target = nlohmann::json::array();
                target.push_back(server["host"].get<std::string>());
                target.push_back(server["port"].get<uint16_t>());
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

        cfg.routes.clear();
        if (!build_routes_from_legacy_proxies(json, cfg.routes)) {
            return false;
        }
        if (!build_routes_from_upstreams(json, cfg.routes)) {
            return false;
        }

        if (!parse_static_mounts(json, cfg)) {
            return false;
        }

        if (cfg.routes.empty()) {
            std::cerr << "at least one route is required (proxies[] or routes[] with upstreams{})\n";
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
    }

    void install_routes(yuan::net::http::HttpProxy *proxy, const std::vector<nlohmann::json> &routes)
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
            if (route_json.contains("pool_size") && route_json["pool_size"].is_number_unsigned()) {
                route.max_pool_size_per_target = route_json["pool_size"].get<size_t>();
            }
            if (route_json.contains("idle_timeout") && route_json["idle_timeout"].is_number_unsigned()) {
                route.idle_timeout_seconds = route_json["idle_timeout"].get<size_t>();
            }

            proxy->add_route(route);
        }
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

            std::lock_guard<std::mutex> guard(*lock);
            if (json_format) {
                nlohmann::json line;
                line["ts"] = ts;
                line["ip"] = ip;
                line["method"] = method.empty() ? "-" : method;
                line["url"] = url.empty() ? "/" : url;
                line["status"] = status;
                line["upstream"] = upstream;
                line["latency_ms"] = 0;
                (*stream) << line.dump() << '\n';
            } else {
                (*stream) << ts
                          << " ip=" << ip
                          << " method=" << (method.empty() ? "-" : method)
                          << " url=" << (url.empty() ? "/" : url)
                          << " status=" << status
                          << " upstream=" << upstream
                          << " latency_ms=0"
                          << '\n';
            }
            stream->flush();
        });

        std::cout << "access log enabled: " << cfg.access_log_path << '\n';
    }

    bool reload_routes(yuan::net::http::HttpProxy *proxy,
                       const std::string &config_path,
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
        active_cfg.routes = std::move(next_cfg.routes);
        active_cfg.reload_check_interval_ms = next_cfg.reload_check_interval_ms;

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

    std::string config_path = "server/mini_nginx/mini_nginx.json";
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

    yuan::app::Application application(context);
    auto http_service = std::make_shared<yuan::server::HttpService>(cfg.listen_port, cfg.server_config);
    install_access_log(*http_service, cfg);
    if (!application.add_typed_service<yuan::server::HttpService>("http", http_service, "server.http", 1)) {
        std::cerr << "failed to register http service\n";
        return 1;
    }

    yuan::app::Bootstrap bootstrap(application);
    if (!bootstrap.run()) {
        std::cerr << "failed to start mini_nginx service\n";
        return 1;
    }

    auto *proxy = http_service->server().ensure_proxy();
    if (!proxy) {
        std::cerr << "http proxy module is unavailable\n";
        bootstrap.shutdown();
        return 1;
    }

    install_static_mounts(http_service->server(), cfg);

    install_routes(proxy, cfg.routes);

    std::cout << "mini_nginx listening on 0.0.0.0:" << cfg.listen_port << " using config " << config_path << '\n';
    std::cout << "routes loaded: " << cfg.routes.size() << '\n';
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
            (void)reload_routes(proxy, config_path, cfg);
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
