#include "web/web_server_service.h"

#include "option.h"
#include "redis_client.h"
#include "internal/def.h"

namespace yuan::game::server
{
    WebServerService::WebServerService(GameServiceId service_id,
                                         std::uint16_t port,
                                         std::uint16_t tunnel_port,
                                         GameServiceId world_id,
                                         std::uint16_t world_port,
                                         std::vector<std::uint16_t> world_ports,
                                         std::string redis_host,
                                         std::uint16_t redis_port,
                                         std::uint16_t redis_db,
                                         std::string redis_username,
                                         std::string redis_password,
                                         std::uint16_t redis_connect_timeout_ms,
                                         std::uint16_t redis_command_timeout_ms,
                                         std::size_t expected_requests)
        : port_(port),
          tunnel_port_(tunnel_port),
          world_port_(world_port),
          world_ports_(std::move(world_ports)),
          expected_requests_(expected_requests),
          service_id_(service_id),
          world_id_(world_id),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms),
          web_({service_id, 600, yuan::game_base::ServerRole::world, service_id.world, "web"})
    {
    }

    void WebServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WebServerService::init()
    {
        web_.set_bootstrap_provider([this](LoginOptionsRequest request) {
            return fetch_bootstrap(request);
        });
        web_.set_register_handler([this](WebAuthRequest request) {
            return register_account(std::move(request));
        });
        web_.set_login_handler([this](WebAuthRequest request) {
            return login_account(std::move(request));
        });
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
        ok_ = rpc_server_.bind_loopback(port_, web_.rpc_server(), expected_requests_);
        return ok_;
    }

    void WebServerService::start()
    {
        ok_ = ok_ && rpc_server_.run();
    }

    void WebServerService::stop()
    {
        rpc_server_.stop();
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
        const auto allocated = redis_->incr("game:account:next_uid");
        if (!allocated) {
            return WebAuthResponse{false, 0, {}, "failed to allocate player uid"};
        }
        const auto uid = static_cast<PlayerUid>(std::stoull(allocated->to_string()));
        const auto existing = redis_->get(account_key);
        if (existing && existing->get_type() != yuan::redis::resp_null) {
            return WebAuthResponse{false, 0, {}, "account already exists"};
        }
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
        yuan::rpc::Bytes world_payload;
        if (!encode_login_options_request(LoginOptionsRequest{player_uid}, world_payload)) {
            return std::nullopt;
        }
        yuan::rpc::Message message;
        message.route = game_route::world_login_options();
        message.payload = std::move(world_payload);
        const auto world_port = select_world_port(player_uid);
        if (world_port == 0) {
            return std::nullopt;
        }
        auto response = rpc_network::RpcNetworkClient().call(rpc_network::RpcEndpoint{"127.0.0.1", world_port}, message);
        if (!response || response->status != yuan::rpc::RpcStatus::ok) {
            return std::nullopt;
        }
        return decode_login_options_response(response->payload);
    }

    std::uint16_t WebServerService::select_world_port(PlayerUid player_uid) const
    {
        if (!world_ports_.empty()) {
            return world_ports_[static_cast<std::size_t>(player_uid % world_ports_.size())];
        }
        return world_port_;
    }

}
