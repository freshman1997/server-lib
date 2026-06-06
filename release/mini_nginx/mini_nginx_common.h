#pragma once

#include "application.h"
#include "authorization.h"
#include "base/utils/base64.h"
#include "bootstrap.h"
#include "context.h"
#include "header_key.h"
#include "http/http_service.h"
#include "middleware.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "net/socket/listen_options.h"
#include "nlohmann/json.hpp"
#include "ops/option.h"
#include "proxy.h"
#include "request.h"
#include "response.h"

#include "openssl/sha.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mini_nginx
{
    struct CliOptions {
        std::string config_path = "release/mini_nginx/mini_nginx.json";
        bool test_config = false;
        bool help = false;
    };

    struct RedirectRule {
        std::string from;
        std::string to;
        int code = 302;
        bool prefix = false;
        bool preserve_path = false;
    };

    struct ReturnRule {
        std::string path;
        bool exact = false;
        int code = 200;
        std::string body;
        std::string location;
        std::string content_type = "text/plain; charset=utf-8";
    };

    enum class RewriteMatchMode {
        prefix,
        exact,
        regex,
    };

    struct RewriteRule {
        std::string from;
        std::string to;
        RewriteMatchMode match = RewriteMatchMode::regex;
        bool case_sensitive = true;
        bool preserve_query = true;
        bool preserve_path = true;
        int code = 0;
        std::regex compiled;
    };

    struct BasicAuthRule {
        std::string path = "/";
        bool exact = false;
        bool enabled = true;
        std::string realm = "Restricted";
        std::unordered_map<std::string, std::string> users;
    };

    enum class SatisfyMode {
        all,
        any,
    };

    struct MiniNginxConfig {
        MiniNginxConfig()
        {
            server_config.enable_ssl = false;
        }

        yuan::net::http::HttpServerConfig server_config;
        int listen_port = 8080;
        bool access_log_enabled = true;
        bool access_log_json = true;
        std::string access_log_path = "release/mini_nginx/access.log";
        std::string access_log_format;
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
        bool server_tokens = true;
        std::string server_header = "mini_nginx";
        std::vector<std::pair<std::string, std::string>> response_headers;
        std::vector<RedirectRule> redirects;
        std::vector<ReturnRule> return_rules;
        std::vector<RewriteRule> rewrite_rules;
        std::vector<BasicAuthRule> basic_auth_rules;
        SatisfyMode satisfy_mode = SatisfyMode::all;
        std::unordered_map<int, yuan::net::http::StaticMountOptions::ErrorPage> static_error_pages;
        std::unordered_set<std::string> allowed_methods;
        std::vector<yuan::net::http::AccessRule> access_rules;
        std::vector<yuan::net::http::StaticMount> static_mounts;
        std::vector<nlohmann::json> routes;
    };

    int read_env_int(const char *name, int default_value);
    std::string read_env_string(const char *name, const std::string &default_value = {});
    bool json_bool(const nlohmann::json &json, const char *key, bool fallback);
    int json_int(const nlohmann::json &json, const char *key, int fallback);
    std::string to_upper_ascii(std::string value);
    std::string to_lower_ascii(std::string value);
    yuan::net::ListenSchedulingMode parse_listen_scheduling_mode(
        const std::string &value,
        yuan::net::ListenSchedulingMode fallback);
    std::string normalize_extension(std::string ext);
    std::string join_methods(const std::unordered_set<std::string> &methods);
    std::string trim_ascii(std::string value);
    std::string remote_ip_from_request(yuan::net::http::HttpRequest *req);
    bool access_allowed(yuan::net::http::HttpRequest *req,
                        const std::vector<yuan::net::http::AccessRule> &rules);

    void print_usage(const char *program);
    bool parse_cli(int argc, char **argv, CliOptions &options);

    bool route_match_is_regex(const nlohmann::json &route);
    bool validate_route(const nlohmann::json &route, std::string &error);
    bool parse_header_map(const nlohmann::json &obj,
                          std::vector<std::pair<std::string, std::string>> &headers);
    bool parse_proxy_redirects(const nlohmann::json &value,
                               std::vector<yuan::net::http::ProxyRedirectRule> &redirects,
                               const char *field_name);
    bool parse_string_array(const nlohmann::json &arr,
                            std::vector<std::string> &values,
                            const char *field_name);
    bool parse_method_set(const nlohmann::json &arr,
                          std::unordered_set<std::string> &methods,
                          const char *field_name);
    bool parse_access_rules(const nlohmann::json &json,
                            std::vector<yuan::net::http::AccessRule> &rules,
                            const char *scope);
    bool parse_duration_ms(const nlohmann::json &value, int &milliseconds);
    bool parse_byte_size(const nlohmann::json &value, std::size_t &bytes);
    bool validate_loaded_config(const MiniNginxConfig &cfg);
    bool load_and_apply_config(const std::string &path, MiniNginxConfig &cfg);
    void apply_env_overrides(MiniNginxConfig &cfg);

    bool path_matches_return_rule(const std::string &path, const ReturnRule &rule);
    const BasicAuthRule *select_basic_auth_rule(const std::string &path,
                                                const std::vector<BasicAuthRule> &rules);
    bool basic_auth_allowed(yuan::net::http::HttpRequest *req,
                            const BasicAuthRule &rule);
    void send_basic_auth_challenge(yuan::net::http::HttpRequest *req,
                                   yuan::net::http::HttpResponse *resp,
                                   const BasicAuthRule &rule);
    bool apply_rewrite_rules(yuan::net::http::HttpRequest *req,
                             yuan::net::http::HttpResponse *resp,
                             const std::vector<RewriteRule> &rules);
    void send_return_response(yuan::net::http::HttpRequest *req,
                              yuan::net::http::HttpResponse *resp,
                              const ReturnRule &rule);

    std::shared_ptr<yuan::server::HttpService> create_http_service(const MiniNginxConfig &cfg);
    bool reload_routes(yuan::net::http::HttpProxyHandler *proxy,
                       const std::string &config_path,
                       yuan::net::http::HttpServer &server,
                       MiniNginxConfig &active_cfg);
}
