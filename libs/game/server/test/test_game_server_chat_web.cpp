#include "chat_web/service/chat_service.h"

#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using namespace yuan::game::server;

    yuan::redis::Option option;
    option.host_ = "127.0.0.1";
    option.port_ = 6379;
    option.db_ = 0;
    option.timeout_ms_ = 500;
    option.connect_timeout_ms_ = 500;
    option.command_timeout_ms_ = 500;
    option.name_ = "game-chat-web-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_chat_web skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string user_a = "chat-test-a-" + suffix;
    const std::string user_b = "chat-test-b-" + suffix;
    const std::string group_id = "group-" + suffix;
    const std::string custom_channel = "custom-" + suffix;

    ChatService chat(redis);
    auto result = chat.add_friend(ChatFriendRequest{user_a, user_b});
    if (!require(result.ok(), "friend add should succeed")) {
        return 1;
    }
    result = chat.list_friends(ChatFriendListRequest{user_a});
    if (!require(result.ok() && !result.body["friends"].empty(), "friend list should include friend")) {
        return 2;
    }

    result = chat.publish(ChatPublishRequest{ChatConversationType::world, {}, {}, user_a, {}, "hello-world", "text"});
    if (!require(result.ok() && result.body["message"]["channel"] == "world", "world publish should use world channel")) {
        return 3;
    }
    const auto world_message_id = result.body["message"].value("message_id", std::string{});
    result = chat.messages(ChatMessagesRequest{ChatConversationType::world, {}, {}, {}, {}, 10});
    if (!require(result.ok() && !result.body["messages"].empty(), "world messages should return published message")) {
        return 4;
    }

    result = chat.publish(ChatPublishRequest{ChatConversationType::group, {}, group_id, user_a, {}, "hello-group", "image"});
    if (!require(result.ok() && result.body["message"]["channel"] == "group:" + group_id, "group publish should use group channel")) {
        return 5;
    }
    const auto group_message_id = result.body["message"].value("message_id", std::string{});
    result = chat.messages(ChatMessagesRequest{ChatConversationType::group, {}, group_id, {}, {}, 10});
    if (!require(result.ok() && !result.body["messages"].empty(), "group messages should return published message")) {
        return 6;
    }

    result = chat.publish(ChatPublishRequest{ChatConversationType::private_chat, {}, {}, user_a, user_b, "hello-private", "voice"});
    if (!require(result.ok() && result.body["message"]["to_user_id"] == user_b, "private publish should include target user")) {
        return 7;
    }
    const auto private_message_id = result.body["message"].value("message_id", std::string{});
    result = chat.messages(ChatMessagesRequest{ChatConversationType::private_chat, {}, {}, user_b, user_a, 10});
    if (!require(result.ok() && !result.body["messages"].empty(), "private messages should be readable from either side")) {
        return 8;
    }

    result = chat.publish(ChatPublishRequest{ChatConversationType::channel, custom_channel, {}, user_a, {}, "bad", "unsupported"});
    if (!require(result.status == ChatServiceStatus::bad_request, "unsupported message type should be rejected")) {
        return 9;
    }

    result = chat.remove_friend(ChatFriendRequest{user_a, user_b});
    if (!require(result.ok(), "friend remove should succeed")) {
        return 10;
    }

    (void)redis->command("LREM", {"game:chat:channel:world:messages", "0", world_message_id});
    (void)redis->command("LREM", {"game:chat:channel:group:" + group_id + ":messages", "0", group_message_id});
    (void)redis->command("LREM", {"game:chat:channel:private:" + user_a + ":" + user_b + ":messages", "0", private_message_id});
    (void)redis->command("DEL", {"game:chat:friend:" + user_a,
                                  "game:chat:friend:" + user_b,
                                  "game:chat:message:" + world_message_id,
                                  "game:chat:message:" + group_message_id,
                                  "game:chat:message:" + private_message_id});
    return 0;
}
