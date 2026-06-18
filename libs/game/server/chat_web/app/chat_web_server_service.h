#ifndef YUAN_GAME_SERVER_CHAT_WEB_APP_CHAT_WEB_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_CHAT_WEB_APP_CHAT_WEB_SERVER_SERVICE_H

#include "application.h"
#include "chat_web/handler/chat_handler.h"
#include "chat_web/service/chat_service.h"

#include "http_server.h"
#include "redis_client.h"

#include <memory>
#include <string>

namespace yuan::game::server
{
    class ChatWebServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        ChatWebServerService(std::string listen_host,
                             std::uint16_t port,
                             std::string redis_host,
                             std::uint16_t redis_port,
                             std::uint16_t redis_db,
                             std::string redis_username,
                             std::string redis_password,
                             std::uint16_t redis_connect_timeout_ms,
                             std::uint16_t redis_command_timeout_ms);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;
        [[nodiscard]] bool ok() const;

    private:
        yuan::app::RuntimeContext context_;
        std::string listen_host_;
        std::uint16_t port_ = 0;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::unique_ptr<ChatService> chat_service_;
        ChatHandlerContext chat_context_;
        std::unique_ptr<yuan::net::http::HttpServer> http_server_;
        bool ok_ = false;
    };
}

#endif
