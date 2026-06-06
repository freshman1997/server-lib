#include "mini_nginx_common.h"

namespace mini_nginx
{
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

        const std::string scheduling_mode = read_env_string("YUAN_MINI_NGINX_SCHEDULING_MODE");
        if (!scheduling_mode.empty()) {
            cfg.server_config.listen_options.scheduling_mode =
                parse_listen_scheduling_mode(scheduling_mode,
                                             cfg.server_config.listen_options.scheduling_mode);
        }

        const int shard_count = read_env_int("YUAN_MINI_NGINX_SHARD_COUNT",
                                             static_cast<int>(cfg.server_config.listen_options.shard_count));
        if (shard_count > 0) {
            cfg.server_config.listen_options.shard_count = static_cast<std::size_t>(shard_count);
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

            auto apply_match_string = [&](const char *key) {
                if (!route_json.contains(key) || !route_json[key].is_string()) {
                    return;
                }
                const auto match = to_lower_ascii(trim_ascii(route_json[key].get<std::string>()));
                if (match == "exact" || match == "=") {
                    route.match_type = yuan::net::http::ProxyRoute::MatchType::exact;
                } else if (match == "prefix" || match == "prefix_strong" || match == "^~") {
                    route.match_type = yuan::net::http::ProxyRoute::MatchType::prefix;
                    if (match == "prefix_strong" || match == "^~") {
                        route.match_type = yuan::net::http::ProxyRoute::MatchType::prefix_strong;
                    }
                } else if (match == "regex" || match == "~") {
                    route.match_type = yuan::net::http::ProxyRoute::MatchType::regex;
                    route.regex_case_sensitive = true;
                    route.strip_prefix = false;
                } else if (match == "regex_i" || match == "~*") {
                    route.match_type = yuan::net::http::ProxyRoute::MatchType::regex;
                    route.regex_case_sensitive = false;
                    route.strip_prefix = false;
                }
            };
            if (route_json.contains("exact") && route_json["exact"].is_boolean() && route_json["exact"].get<bool>()) {
                route.match_type = yuan::net::http::ProxyRoute::MatchType::exact;
            }
            apply_match_string("match");
            apply_match_string("location_match");

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
            auto apply_route_duration = [&](const char *key, int &target) {
                if (!route_json.contains(key)) {
                    return;
                }
                int ms = 0;
                if (parse_duration_ms(route_json[key], ms)) {
                    target = ms;
                } else {
                    std::cerr << "route." << key << " must be milliseconds or a duration like 30s\n";
                }
            };
            apply_route_duration("connect_timeout", route.connect_timeout_ms);
            apply_route_duration("proxy_connect_timeout", route.connect_timeout_ms);
            apply_route_duration("read_timeout", route.read_timeout_ms);
            apply_route_duration("proxy_read_timeout", route.read_timeout_ms);
            apply_route_duration("write_timeout", route.write_timeout_ms);
            apply_route_duration("proxy_send_timeout", route.write_timeout_ms);
            if (route_json.contains("idle_timeout")) {
                if (route_json["idle_timeout"].is_number_unsigned()) {
                    route.idle_timeout_seconds = route_json["idle_timeout"].get<size_t>();
                } else if (route_json["idle_timeout"].is_number_integer()) {
                    route.idle_timeout_seconds = static_cast<std::size_t>((std::max)(1, route_json["idle_timeout"].get<int>()));
                } else {
                    int idle_ms = 0;
                    if (parse_duration_ms(route_json["idle_timeout"], idle_ms)) {
                        route.idle_timeout_seconds = static_cast<std::size_t>((std::max)(1, idle_ms / 1000));
                    }
                }
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
            if (route_json.contains("proxy_cache") && route_json["proxy_cache"].is_boolean()) {
                route.cache_enabled = route_json["proxy_cache"].get<bool>();
            }
            auto apply_cache_duration = [&](const char *key) {
                if (!route_json.contains(key)) {
                    return true;
                }
                int ms = 0;
                if (!parse_duration_ms(route_json[key], ms)) {
                    std::cerr << "route." << key << " must be milliseconds or a duration like 60s\n";
                    return false;
                }
                route.cache_ttl_ms = ms;
                return true;
            };
            if (!apply_cache_duration("proxy_cache_valid") ||
                !apply_cache_duration("proxy_cache_ttl")) {
                continue;
            }
            if (route_json.contains("proxy_cache_max_size")) {
                std::size_t bytes = 0;
                if (!parse_byte_size(route_json["proxy_cache_max_size"], bytes)) {
                    std::cerr << "route.proxy_cache_max_size must be bytes or a size like 256k\n";
                    continue;
                }
                route.cache_max_response_bytes = bytes;
            }
            if (route_json.contains("proxy_cache_methods")) {
                std::unordered_set<std::string> methods;
                if (!parse_method_set(route_json["proxy_cache_methods"], methods, "route.proxy_cache_methods") ||
                    methods.empty()) {
                    std::cerr << "route.proxy_cache_methods is invalid\n";
                    continue;
                }
                route.cache_methods = std::move(methods);
            }
            if (route_json.contains("proxy_cache_key") && route_json["proxy_cache_key"].is_string()) {
                route.cache_key_template = route_json["proxy_cache_key"].get<std::string>();
            }
            if (route_json.contains("proxy_cache_bypass_headers") &&
                !parse_string_array(route_json["proxy_cache_bypass_headers"], route.cache_bypass_headers, "proxy_cache_bypass_headers")) {
                std::cerr << "route.proxy_cache_bypass_headers is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_no_cache_headers") &&
                !parse_string_array(route_json["proxy_no_cache_headers"], route.cache_no_cache_headers, "proxy_no_cache_headers")) {
                std::cerr << "route.proxy_no_cache_headers is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_cache_ignore_cache_control") &&
                route_json["proxy_cache_ignore_cache_control"].is_boolean()) {
                route.cache_ignore_cache_control = route_json["proxy_cache_ignore_cache_control"].get<bool>();
            }
            if (route_json.contains("proxy_cache_ignore_set_cookie") &&
                route_json["proxy_cache_ignore_set_cookie"].is_boolean()) {
                route.cache_ignore_set_cookie = route_json["proxy_cache_ignore_set_cookie"].get<bool>();
            }
            if (route_json.contains("preserve_host") && route_json["preserve_host"].is_boolean()) {
                route.preserve_host = route_json["preserve_host"].get<bool>();
            }
            if (route_json.contains("proxy_preserve_host") && route_json["proxy_preserve_host"].is_boolean()) {
                route.preserve_host = route_json["proxy_preserve_host"].get<bool>();
            }
            if (route_json.contains("proxy_set_header") &&
                !parse_header_map(route_json["proxy_set_header"], route.request_headers)) {
                std::cerr << "route.proxy_set_header is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_headers") &&
                !parse_header_map(route_json["proxy_headers"], route.request_headers)) {
                std::cerr << "route.proxy_headers is invalid\n";
                continue;
            }
            if (route_json.contains("hide_request_headers") &&
                !parse_string_array(route_json["hide_request_headers"], route.hide_request_headers, "hide_request_headers")) {
                std::cerr << "route.hide_request_headers is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_hide_request_headers") &&
                !parse_string_array(route_json["proxy_hide_request_headers"], route.hide_request_headers, "proxy_hide_request_headers")) {
                std::cerr << "route.proxy_hide_request_headers is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_set_response_header") &&
                !parse_header_map(route_json["proxy_set_response_header"], route.response_headers)) {
                std::cerr << "route.proxy_set_response_header is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_response_headers") &&
                !parse_header_map(route_json["proxy_response_headers"], route.response_headers)) {
                std::cerr << "route.proxy_response_headers is invalid\n";
                continue;
            }
            if (route_json.contains("hide_response_headers") &&
                !parse_string_array(route_json["hide_response_headers"], route.hide_response_headers, "hide_response_headers")) {
                std::cerr << "route.hide_response_headers is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_hide_header") &&
                !parse_string_array(route_json["proxy_hide_header"], route.hide_response_headers, "proxy_hide_header")) {
                std::cerr << "route.proxy_hide_header is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_redirect") &&
                !parse_proxy_redirects(route_json["proxy_redirect"], route.proxy_redirects, "route.proxy_redirect")) {
                std::cerr << "route.proxy_redirect is invalid\n";
                continue;
            }
            if (route_json.contains("proxy_redirects") &&
                !parse_proxy_redirects(route_json["proxy_redirects"], route.proxy_redirects, "route.proxy_redirects")) {
                std::cerr << "route.proxy_redirects is invalid\n";
                continue;
            }
            if (route_json.contains("allowed_methods") &&
                !parse_method_set(route_json["allowed_methods"], route.allowed_methods, "route.allowed_methods")) {
                std::cerr << "route.allowed_methods is invalid\n";
                continue;
            }
            if (route_json.contains("limit_except") &&
                !parse_method_set(route_json["limit_except"], route.allowed_methods, "route.limit_except")) {
                std::cerr << "route.limit_except is invalid\n";
                continue;
            }
            if (!parse_access_rules(route_json, route.access_rules, "route")) {
                std::cerr << "route access rules are invalid\n";
                continue;
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
        if (cfg.allowed_methods.empty() && cfg.response_headers.empty() &&
            cfg.redirects.empty() && cfg.return_rules.empty() &&
            cfg.rewrite_rules.empty() && cfg.basic_auth_rules.empty() && cfg.access_rules.empty() &&
            !cfg.server_tokens) {
            return;
        }

        auto allowed = cfg.allowed_methods;
        auto allow_header = join_methods(allowed);
        auto server_tokens = cfg.server_tokens;
        auto server_header = cfg.server_header;
        auto headers = cfg.response_headers;
        auto redirects = cfg.redirects;
        auto return_rules = cfg.return_rules;
        auto rewrite_rules = cfg.rewrite_rules;
        auto basic_auth_rules = cfg.basic_auth_rules;
        auto access_rules = cfg.access_rules;
        auto satisfy_mode = cfg.satisfy_mode;

        http_service.server().use(
            [allowed = std::move(allowed),
             allow_header = std::move(allow_header),
             server_tokens,
             server_header = std::move(server_header),
             headers = std::move(headers),
             redirects = std::move(redirects),
             return_rules = std::move(return_rules),
             rewrite_rules = std::move(rewrite_rules),
             basic_auth_rules = std::move(basic_auth_rules),
             access_rules = std::move(access_rules),
             satisfy_mode](yuan::net::http::HttpRequest *req,
                           yuan::net::http::HttpResponse *resp) {
                if (!req || !resp) {
                    return yuan::net::http::MiddlewareResult::next;
                }

                if (server_tokens) {
                    resp->add_header("Server", server_header);
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
                        return yuan::net::http::MiddlewareResult::stop;
                    }
                }

                if (apply_rewrite_rules(req, resp, rewrite_rules)) {
                    return yuan::net::http::MiddlewareResult::stop;
                }

                const auto path_view = req->get_path();
                const std::string path(path_view.data(), path_view.size());
                const auto *auth_rule = select_basic_auth_rule(path, basic_auth_rules);
                const bool has_auth_module = auth_rule && auth_rule->enabled;
                const bool has_access_module = !access_rules.empty();
                if (satisfy_mode == SatisfyMode::any && has_auth_module && has_access_module) {
                    const bool access_ok = access_allowed(req, access_rules);
                    const bool auth_ok = basic_auth_allowed(req, *auth_rule);
                    if (!access_ok && !auth_ok) {
                        send_basic_auth_challenge(req, resp, *auth_rule);
                        return yuan::net::http::MiddlewareResult::stop;
                    }
                } else {
                    if (!access_allowed(req, access_rules)) {
                        resp->process_error(yuan::net::http::ResponseCode::forbidden);
                        return yuan::net::http::MiddlewareResult::stop;
                    }
                    if (has_auth_module && !basic_auth_allowed(req, *auth_rule)) {
                        send_basic_auth_challenge(req, resp, *auth_rule);
                        return yuan::net::http::MiddlewareResult::stop;
                    }
                }

                for (const auto &rule : return_rules) {
                    if (!path_matches_return_rule(path, rule)) {
                        continue;
                    }
                    send_return_response(req, resp, rule);
                    return yuan::net::http::MiddlewareResult::stop;
                }

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
        server.on(path, [json](yuan::net::http::HttpRequest *, yuan::net::http::HttpResponse *resp) {
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

    void replace_all_literal(std::string &value, const std::string &from, const std::string &to)
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

    std::string format_seconds_ms(uint64_t elapsed_ms)
    {
        std::ostringstream oss;
        oss << (elapsed_ms / 1000) << '.'
            << std::setw(3) << std::setfill('0') << (elapsed_ms % 1000);
        return oss.str();
    }

    std::string expand_access_log_format(const std::string &format,
                                         yuan::net::http::HttpRequest *req,
                                         yuan::net::http::HttpResponse *resp,
                                         const std::string &ts,
                                         int status,
                                         uint64_t latency_ms)
    {
        std::string out = format;
        const std::string method = req ? req->get_raw_method() : "";
        const std::string url = req ? req->get_raw_url() : "";
        const std::string path = req ? std::string(req->get_path()) : "";
        const std::string version = req ? req->get_raw_version() : "HTTP/1.1";
        const std::string request_line =
            (method.empty() ? "-" : method) + " " +
            (url.empty() ? "/" : url) + " " +
            (version.empty() ? "HTTP/1.1" : version);
        const std::string remote = req ? remote_ip_from_request(req) : "-";
        const std::string host = req && req->get_header("host") ? *req->get_header("host") : "-";
        const std::string referer = req && req->get_header("referer") ? *req->get_header("referer") : "-";
        const std::string user_agent = req && req->get_header("user-agent") ? *req->get_header("user-agent") : "-";
        const std::string bytes = resp ? std::to_string(resp->body_buffer_size()) : "0";

        replace_all_literal(out, "$remote_addr", remote.empty() ? "-" : remote);
        replace_all_literal(out, "$time_local", ts);
        replace_all_literal(out, "$request", request_line);
        replace_all_literal(out, "$request_method", method.empty() ? "-" : method);
        replace_all_literal(out, "$request_uri", url.empty() ? "/" : url);
        replace_all_literal(out, "$uri", path.empty() ? "/" : path);
        replace_all_literal(out, "$status", std::to_string(status));
        replace_all_literal(out, "$body_bytes_sent", bytes);
        replace_all_literal(out, "$host", host);
        replace_all_literal(out, "$http_host", host);
        replace_all_literal(out, "$http_referer", referer);
        replace_all_literal(out, "$http_user_agent", user_agent);
        replace_all_literal(out, "$request_time", format_seconds_ms(latency_ms));
        return out;
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
        const std::string custom_format = cfg.access_log_format;
        http_service.server().set_access_log_hook([stream, lock, json_format, custom_format](yuan::net::http::HttpRequest *req,
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
                if (!custom_format.empty()) {
                    (*stream) << expand_access_log_format(custom_format, req, resp, ts, status, latency_ms) << '\n';
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

    bool reload_routes(yuan::net::http::HttpProxyHandler *proxy, const std::string &config_path, yuan::net::http::HttpServer &server, MiniNginxConfig &active_cfg)
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
