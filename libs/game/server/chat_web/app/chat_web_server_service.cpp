#include "chat_web/app/chat_web_server_service.h"

#include "option.h"

namespace yuan::game::server
{
    ChatWebServerService::ChatWebServerService(std::string listen_host,
                                               std::uint16_t port,
                                               std::string redis_host,
                                               std::uint16_t redis_port,
                                               std::uint16_t redis_db,
                                               std::string redis_username,
                                               std::string redis_password,
                                               std::uint16_t redis_connect_timeout_ms,
                                               std::uint16_t redis_command_timeout_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms)
    {
    }

    void ChatWebServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool ChatWebServerService::init()
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
        option.name_ = "game-chat-web";
        redis_ = std::make_shared<yuan::redis::RedisClient>(option);
        chat_service_ = std::make_unique<ChatService>(redis_);
        chat_context_.subscribe_handler = [this](ChatSubscriptionRequest request) {
            return chat_service_->subscribe(std::move(request));
        };
        chat_context_.unsubscribe_handler = [this](ChatSubscriptionRequest request) {
            return chat_service_->unsubscribe(std::move(request));
        };
        chat_context_.publish_handler = [this](ChatPublishRequest request) {
            return chat_service_->publish(std::move(request));
        };
        chat_context_.recall_handler = [this](ChatRecallRequest request) {
            return chat_service_->recall(std::move(request));
        };
        chat_context_.messages_handler = [this](ChatMessagesRequest request) {
            return chat_service_->messages(std::move(request));
        };
        chat_context_.add_friend_handler = [this](ChatFriendRequest request) {
            return chat_service_->add_friend(std::move(request));
        };
        chat_context_.remove_friend_handler = [this](ChatFriendRequest request) {
            return chat_service_->remove_friend(std::move(request));
        };
        chat_context_.list_friends_handler = [this](ChatFriendListRequest request) {
            return chat_service_->list_friends(std::move(request));
        };

        yuan::net::http::HttpServerConfig http_config;
        http_config.enable_keep_alive = false;
        http_config.server_name = "GameChatWeb/1.0";
        http_server_ = std::make_unique<yuan::net::http::HttpServer>(http_config);
        if (!register_chat_http_handlers(*http_server_, chat_context_)) {
            return false;
        }
        ok_ = http_server_->init(port_);
        return ok_;
    }

    void ChatWebServerService::start()
    {
        if (ok_ && http_server_) {
            http_server_->serve();
        }
    }

    void ChatWebServerService::stop()
    {
        if (http_server_) {
            http_server_->stop();
        }
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
    }

    bool ChatWebServerService::ok() const
    {
        return ok_;
    }

}
