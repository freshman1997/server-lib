#include "web/app/web_server_service.h"

#include "option.h"
#include "redis_client.h"
#include "internal/def.h"

#include "http_client.h"
#include "request.h"
#include "response.h"

#include "coroutine/sync_wait.h"

#include "logger.h"

namespace yuan::game::server
{
    WebServerService::WebServerService(std::string listen_host,
                                          std::uint16_t port,
                                          std::string world_host,
                                          std::uint16_t world_port,
                                          std::vector<std::uint16_t> world_ports,
                                         std::string redis_host,
                                         std::uint16_t redis_port,
                                         std::uint16_t redis_db,
                                         std::string redis_username,
                                          std::string redis_password,
                                          std::uint16_t redis_connect_timeout_ms,
                                          std::uint16_t redis_command_timeout_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          world_host_(std::move(world_host)),
          world_port_(world_port),
          world_ports_(std::move(world_ports)),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms)
    {
    }

    void WebServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WebServerService::init()
    {
        web_context_.bootstrap_provider = [this](LoginOptionsRequest request) {
            return fetch_bootstrap(request);
        };
        web_context_.register_handler = [this](WebAuthRequest request) {
            return register_account(std::move(request));
        };
        web_context_.login_handler = [this](WebAuthRequest request) {
            return login_account(std::move(request));
        };
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
        yuan::net::http::HttpServerConfig http_config;
        http_config.enable_keep_alive = false;
        http_config.server_name = "GameWeb/1.0";
        http_server_ = std::make_unique<yuan::net::http::HttpServer>(http_config);
        if (!register_web_http_handlers(*http_server_, web_context_)) {
            return false;
        }
        ok_ = http_server_->init(port_);
        return ok_;
    }

    void WebServerService::start()
    {
        if (ok_ && http_server_) {
            http_thread_ = std::jthread([this](std::stop_token) {
                http_server_->serve();
            });
        }
    }

    void WebServerService::stop()
    {
        if (http_server_) {
            http_server_->stop();
        }
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
    }

    bool WebServerService::ok() const
    {
        return ok_;
    }

    std::optional<LoginOptionsResponse> WebServerService::fetch_bootstrap(LoginOptionsRequest request) const
    {
        return fetch_login_options(request.player_uid);
    }

    WebAuthResponse WebServerService::register_account(WebAuthRequest request) const
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

    WebAuthResponse WebServerService::login_account(WebAuthRequest request) const
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

    std::optional<LoginOptionsResponse> WebServerService::fetch_login_options(PlayerUid player_uid) const
    {
        const auto world_port = select_world_port(player_uid);
        if (world_port == 0) {
            return std::nullopt;
        }
        yuan::net::http::HttpClient http_client;
        if (!http_client.query("http://" + world_host_ + ":" + std::to_string(world_port))) {
            return std::nullopt;
        }
        yuan::net::NetworkRuntime runtime;
        auto *http_response = yuan::coroutine::sync_wait(runtime.runtime_view(),
            http_client.connect_async(runtime.runtime_view(), [player_uid, this](yuan::net::http::HttpRequest *request) {
                request->set_method(yuan::net::http::HttpMethod::get_);
                request->set_raw_url("/game/login_options?player_uid=" + std::to_string(player_uid));
                request->add_header("Connection", "close");
                request->add_header("Host", world_host_);
                request->send();
            }, redis_command_timeout_ms_));
        if (!http_response || !http_response->good() || http_response->get_response_code() != yuan::net::http::ResponseCode::ok_) {
            LOG_ERROR("web failed to fetch world login options uid={} world={}:{}", player_uid, world_host_, world_port);
            return std::nullopt;
        }
        const auto *body = http_response->body_begin();
        return decode_login_options_response_json(body ? std::string(body, http_response->get_body_length()) : std::string{});
    }

    std::uint16_t WebServerService::select_world_port(PlayerUid player_uid) const
    {
        if (!world_ports_.empty()) {
            return world_ports_[static_cast<std::size_t>(player_uid % world_ports_.size())];
        }
        return world_port_;
    }

}
