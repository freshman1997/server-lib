#include "web/service/web_auth_service.h"

#include "option.h"
#include "redis_client.h"
#include "value/null_value.h"

#include "http_client.h"
#include "request.h"
#include "response.h"

#include "coroutine/sync_wait.h"

#include "logger.h"

#include <nlohmann/json.hpp>

namespace yuan::game::server
{
    WebAuthService::WebAuthService(std::string world_host,
                                std::uint16_t world_port,
                                std::vector<std::uint16_t> world_ports,
                                std::vector<WorldEndpointConfig> world_endpoints,
                                WorldRoutingConfig world_routing,
                                std::string redis_host,
                               std::uint16_t redis_port,
                               std::uint16_t redis_db,
                               std::string redis_username,
                               std::string redis_password,
                               std::uint16_t redis_connect_timeout_ms,
                               std::uint16_t redis_command_timeout_ms)
        : world_host_(std::move(world_host)),
          world_port_(world_port),
          world_ports_(std::move(world_ports)),
          world_endpoints_(std::move(world_endpoints)),
          world_routing_(std::move(world_routing)),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms)
    {
    }

    bool WebAuthService::init()
    {
        yuan::redis::Option option;
        option.host_ = redis_host_;
        option.port_ = redis_port_;
        option.db_ = redis_db_;
        option.username_ = redis_username_;
        option.password_ = redis_password_;
        option.timeout_ms_ = redis_command_timeout_ms_;
        option.command_timeout_ms_ = redis_command_timeout_ms_;
        option.connect_timeout_ms_ = redis_connect_timeout_ms_;
        option.name_ = "game-web-auth";
        redis_ = std::make_shared<yuan::redis::RedisClient>(option);
        return true;
    }

    std::optional<LoginOptionsResponse> WebAuthService::fetch_bootstrap(LoginOptionsRequest request) const
    {
        return fetch_login_options(request.player_uid);
    }

    WebAuthResponse WebAuthService::register_account(WebAuthRequest request) const
    {
        if (!redis_ || !redis_->ensure_connected()) {
            return WebAuthResponse{false, 0, {}, "redis unavailable"};
        }
        if (request.account.empty() || request.password.empty()) {
            return WebAuthResponse{false, 0, {}, "account and password are required"};
        }
        const auto account_key = "game:account:" + request.account;
        const auto existing = redis_->get(account_key);
        if (existing && existing->get_type() != yuan::redis::resp_null) {
            return WebAuthResponse{false, 0, {}, "account already exists"};
        }
        const auto allocated = redis_->incr("game:account:next_uid");
        if (!allocated) {
            return WebAuthResponse{false, 0, {}, "failed to allocate player uid"};
        }
        const auto uid = static_cast<PlayerUid>(std::stoull(allocated->to_string()));
        const auto created = redis_->set(account_key, std::to_string(uid) + "\n" + request.password);
        if (!created || created->to_string() != "OK") {
            return WebAuthResponse{false, 0, {}, "failed to create account"};
        }
        return WebAuthResponse{true, uid, fetch_login_options(uid).value_or(LoginOptionsResponse{}), "registered"};
    }

    WebAuthResponse WebAuthService::login_account(WebAuthRequest request) const
    {
        if (!redis_ || !redis_->ensure_connected()) {
            return WebAuthResponse{false, 0, {}, "redis unavailable"};
        }
        const auto account_key = "game:account:" + request.account;
        const auto value = redis_->get(account_key);
        if (!value || value->get_type() == yuan::redis::resp_null) {
            return WebAuthResponse{false, 0, {}, "account not found"};
        }
        const auto stored = value->to_string();
        const auto sep = stored.find('\n');
        if (sep == std::string::npos || stored.substr(sep + 1) != request.password) {
            return WebAuthResponse{false, 0, {}, "invalid password"};
        }
        const auto uid = static_cast<PlayerUid>(std::stoull(stored.substr(0, sep)));
        return WebAuthResponse{true, uid, fetch_login_options(uid).value_or(LoginOptionsResponse{}), "login ok"};
    }

    WebCreateRoleResponse WebAuthService::create_role(WebCreateRoleRequest request) const
    {
        if (request.player_uid == 0 || request.name.empty()) {
            return WebCreateRoleResponse{false, request.player_uid, 0, "player_uid and name are required"};
        }
        const auto world_number = route_world_number_by_player_uid(request.player_uid, world_routing_);
        if (!world_number) {
            return WebCreateRoleResponse{false, request.player_uid, 0, "world routing unavailable"};
        }
        std::string world_host = world_host_;
        auto world_port = select_world_port(request.player_uid);
        for (const auto &endpoint : world_endpoints_) {
            if (endpoint.world == *world_number) {
                if (endpoint.state != "open") {
                    return WebCreateRoleResponse{false, request.player_uid, 0, "world unavailable"};
                }
                world_host = endpoint.host;
                world_port = endpoint.port;
                break;
            }
        }
        yuan::net::http::HttpClient http_client;
        if (!http_client.query("http://" + world_host + ":" + std::to_string(world_port))) {
            return WebCreateRoleResponse{false, request.player_uid, 0, "world unavailable"};
        }
        yuan::net::NetworkRuntime runtime;
        const auto url = "/game/create_role?player_uid=" + std::to_string(request.player_uid) + "&name=" + request.name;
        auto *http_response = yuan::coroutine::sync_wait(runtime.runtime_view(),
            http_client.connect_async(runtime.runtime_view(), [url, world_host](yuan::net::http::HttpRequest *http_request) {
                http_request->set_method(yuan::net::http::HttpMethod::get_);
                http_request->set_raw_url(url);
                http_request->add_header("Connection", "close");
                http_request->add_header("Host", world_host);
                http_request->send();
            }, redis_command_timeout_ms_));
        if (!http_response || !http_response->good() || http_response->get_response_code() != yuan::net::http::ResponseCode::ok_) {
            return WebCreateRoleResponse{false, request.player_uid, 0, "world create role failed"};
        }
        const auto *body = http_response->body_begin();
        try {
            const auto root = nlohmann::json::parse(body ? std::string(body, http_response->get_body_length()) : std::string{});
            return WebCreateRoleResponse{root.value("ok", false),
                                         root.value("player_uid", request.player_uid),
                                         root.value("role_id", static_cast<RoleId>(0)),
                                         root.value("message", std::string{})};
        } catch (...) {
            return WebCreateRoleResponse{false, request.player_uid, 0, "invalid world response"};
        }
    }

    std::optional<LoginOptionsResponse> WebAuthService::fetch_login_options(PlayerUid player_uid) const
    {
        const auto world_number = route_world_number_by_player_uid(player_uid, world_routing_);
        if (!world_number) {
            return std::nullopt;
        }
        std::string world_host = world_host_;
        auto world_port = select_world_port(player_uid);
        for (const auto &endpoint : world_endpoints_) {
            if (endpoint.world == *world_number) {
                if (endpoint.state != "open") {
                    return std::nullopt;
                }
                world_host = endpoint.host;
                world_port = endpoint.port;
                break;
            }
        }
        if (world_host.empty() || world_port == 0) {
            return std::nullopt;
        }
        yuan::net::http::HttpClient http_client;
        if (!http_client.query("http://" + world_host + ":" + std::to_string(world_port))) {
            return std::nullopt;
        }
        yuan::net::NetworkRuntime runtime;
        auto *http_response = yuan::coroutine::sync_wait(runtime.runtime_view(),
            http_client.connect_async(runtime.runtime_view(), [player_uid, world_host](yuan::net::http::HttpRequest *request) {
                request->set_method(yuan::net::http::HttpMethod::get_);
                request->set_raw_url("/game/login_options?player_uid=" + std::to_string(player_uid));
                request->add_header("Connection", "close");
                request->add_header("Host", world_host);
                request->send();
            }, redis_command_timeout_ms_));
        if (!http_response || !http_response->good() || http_response->get_response_code() != yuan::net::http::ResponseCode::ok_) {
            LOG_ERROR("web failed to fetch world login options uid={} world={}:{}", player_uid, world_host, world_port);
            return std::nullopt;
        }
        const auto *body = http_response->body_begin();
        return decode_login_options_response_json(body ? std::string(body, http_response->get_body_length()) : std::string{});
    }

    std::uint16_t WebAuthService::select_world_port(PlayerUid player_uid) const
    {
        const auto world_number = route_world_number_by_player_uid(player_uid, world_routing_);
        if (world_number && !world_endpoints_.empty()) {
            for (const auto &endpoint : world_endpoints_) {
                if (endpoint.world == *world_number && endpoint.state == "open") {
                    return endpoint.port;
                }
            }
            return 0;
        }
        if (world_number && !world_ports_.empty()) {
            if (*world_number == 0) {
                return 0;
            }
            const auto index = static_cast<std::size_t>(*world_number - 1);
            return index < world_ports_.size() ? world_ports_[index] : 0;
        }
        if (!world_ports_.empty()) {
            return world_ports_[static_cast<std::size_t>(player_uid % world_ports_.size())];
        }
        return world_port_;
    }
}
