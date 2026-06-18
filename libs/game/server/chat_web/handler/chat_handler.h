#ifndef YUAN_GAME_SERVER_CHAT_WEB_HANDLER_CHAT_HANDLER_H
#define YUAN_GAME_SERVER_CHAT_WEB_HANDLER_CHAT_HANDLER_H

#include "chat_web/service/chat_service_types.h"

#include <functional>

namespace yuan::net::http
{
    class HttpServer;
}

namespace yuan::game::server
{
    struct ChatHandlerContext
    {
        std::function<ChatServiceResult(ChatSubscriptionRequest)> subscribe_handler;
        std::function<ChatServiceResult(ChatSubscriptionRequest)> unsubscribe_handler;
        std::function<ChatServiceResult(ChatPublishRequest)> publish_handler;
        std::function<ChatServiceResult(ChatRecallRequest)> recall_handler;
        std::function<ChatServiceResult(ChatMessagesRequest)> messages_handler;
        std::function<ChatServiceResult(ChatFriendRequest)> add_friend_handler;
        std::function<ChatServiceResult(ChatFriendRequest)> remove_friend_handler;
        std::function<ChatServiceResult(ChatFriendListRequest)> list_friends_handler;
    };

    bool register_chat_http_handlers(yuan::net::http::HttpServer &server, ChatHandlerContext &context);
}

#endif
