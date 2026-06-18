#ifndef YUAN_GAME_SERVER_CHAT_WEB_SERVICE_CHAT_SERVICE_TYPES_H
#define YUAN_GAME_SERVER_CHAT_WEB_SERVICE_CHAT_SERVICE_TYPES_H

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace yuan::game::server
{
    enum class ChatServiceStatus
    {
        ok,
        bad_request,
        not_found,
        unavailable
    };

    struct ChatServiceResult
    {
        ChatServiceStatus status = ChatServiceStatus::ok;
        std::string message;
        nlohmann::json body = nlohmann::json::object();

        [[nodiscard]] bool ok() const
        {
            return status == ChatServiceStatus::ok;
        }
    };

    struct ChatSubscriptionRequest
    {
        std::string channel;
        std::string user_id;
    };

    enum class ChatConversationType
    {
        channel,
        world,
        group,
        private_chat
    };

    struct ChatPublishRequest
    {
        ChatConversationType conversation_type = ChatConversationType::channel;
        std::string channel;
        std::string group_id;
        std::string from_user_id;
        std::string to_user_id;
        std::string text;
        std::string message_type = "text";
    };

    struct ChatRecallRequest
    {
        std::string channel;
        std::string message_id;
    };

    struct ChatMessagesRequest
    {
        ChatConversationType conversation_type = ChatConversationType::channel;
        std::string channel;
        std::string group_id;
        std::string user_id;
        std::string peer_user_id;
        int limit = 50;
    };

    struct ChatFriendRequest
    {
        std::string user_id;
        std::string friend_user_id;
    };

    struct ChatFriendListRequest
    {
        std::string user_id;
    };
}

#endif
