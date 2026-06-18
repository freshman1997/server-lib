#ifndef YUAN_GAME_SERVER_CHAT_WEB_SERVICE_CHAT_SERVICE_H
#define YUAN_GAME_SERVER_CHAT_WEB_SERVICE_CHAT_SERVICE_H

#include "chat_web/service/chat_service_types.h"

#include <memory>
#include <optional>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class ChatService
    {
    public:
        explicit ChatService(std::shared_ptr<yuan::redis::RedisClient> redis);

        ChatServiceResult subscribe(ChatSubscriptionRequest request) const;
        ChatServiceResult unsubscribe(ChatSubscriptionRequest request) const;
        ChatServiceResult publish(ChatPublishRequest request) const;
        ChatServiceResult recall(ChatRecallRequest request) const;
        ChatServiceResult messages(ChatMessagesRequest request) const;
        ChatServiceResult add_friend(ChatFriendRequest request) const;
        ChatServiceResult remove_friend(ChatFriendRequest request) const;
        ChatServiceResult list_friends(ChatFriendListRequest request) const;

    private:
        [[nodiscard]] std::optional<std::string> resolve_conversation_channel(const ChatPublishRequest &request) const;
        [[nodiscard]] std::optional<std::string> resolve_conversation_channel(const ChatMessagesRequest &request) const;
        [[nodiscard]] bool ensure_redis() const;
        [[nodiscard]] std::optional<nlohmann::json> load_message(const std::string &message_id) const;
        bool save_message(const nlohmann::json &message) const;
        void publish_event(const std::string &channel, const nlohmann::json &event) const;

        std::shared_ptr<yuan::redis::RedisClient> redis_;
    };
}

#endif
