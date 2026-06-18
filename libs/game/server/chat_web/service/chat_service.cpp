#include "chat_web/service/chat_service.h"

#include "redis_client.h"
#include "value/array_value.h"
#include "value/null_value.h"

#include <algorithm>

namespace yuan::game::server
{
    namespace
    {
        ChatServiceResult result(ChatServiceStatus status, std::string message, nlohmann::json body = nlohmann::json::object())
        {
            if (!body.contains("ok")) {
                body["ok"] = status == ChatServiceStatus::ok;
            }
            if (!message.empty() && !body.contains("message")) {
                body["message"] = message;
            }
            return ChatServiceResult{status, std::move(message), std::move(body)};
        }

        std::string channel_messages_key(const std::string &channel)
        {
            return "game:chat:channel:" + channel + ":messages";
        }

        std::string channel_subscribers_key(const std::string &channel)
        {
            return "game:chat:channel:" + channel + ":subscribers";
        }

        std::string friend_key(const std::string &user_id)
        {
            return "game:chat:friend:" + user_id;
        }

        std::string message_key(const std::string &message_id)
        {
            return "game:chat:message:" + message_id;
        }

        std::string pubsub_channel(const std::string &channel)
        {
            return "game:chat:pubsub:" + channel;
        }

        std::string private_channel(std::string a, std::string b)
        {
            if (a > b) {
                std::swap(a, b);
            }
            return "private:" + a + ":" + b;
        }

        bool supported_message_type(const std::string &type)
        {
            return type == "text" || type == "image" || type == "voice" || type == "system" || type == "custom";
        }
    }

    ChatService::ChatService(std::shared_ptr<yuan::redis::RedisClient> redis)
        : redis_(std::move(redis))
    {
    }

    ChatServiceResult ChatService::subscribe(ChatSubscriptionRequest request) const
    {
        if (request.channel.empty() || request.user_id.empty()) {
            return result(ChatServiceStatus::bad_request, "channel and user_id are required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        (void)redis_->command("SADD", {channel_subscribers_key(request.channel), request.user_id});
        publish_event(request.channel, { {"type", "subscribe"}, {"channel", request.channel}, {"user_id", request.user_id} });
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"channel", request.channel}, {"user_id", request.user_id}});
    }

    ChatServiceResult ChatService::unsubscribe(ChatSubscriptionRequest request) const
    {
        if (request.channel.empty() || request.user_id.empty()) {
            return result(ChatServiceStatus::bad_request, "channel and user_id are required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        (void)redis_->command("SREM", {channel_subscribers_key(request.channel), request.user_id});
        publish_event(request.channel, { {"type", "unsubscribe"}, {"channel", request.channel}, {"user_id", request.user_id} });
        return result(ChatServiceStatus::ok, {}, {{"ok", true}});
    }

    ChatServiceResult ChatService::publish(ChatPublishRequest request) const
    {
        const auto channel = resolve_conversation_channel(request);
        if (!channel || request.from_user_id.empty() || request.text.empty()) {
            return result(ChatServiceStatus::bad_request, "conversation, from_user_id and text are required");
        }
        if (!supported_message_type(request.message_type)) {
            return result(ChatServiceStatus::bad_request, "unsupported message_type");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        const auto allocated = redis_->incr("game:chat:next_message_id");
        if (!allocated) {
            return result(ChatServiceStatus::unavailable, "failed to allocate message id");
        }
        const auto message_id = allocated->to_string();
        nlohmann::json message{{"message_id", message_id},
                               {"channel", *channel},
                               {"conversation_type", static_cast<int>(request.conversation_type)},
                               {"group_id", request.group_id},
                               {"from_user_id", request.from_user_id},
                               {"to_user_id", request.to_user_id},
                               {"message_type", request.message_type},
                               {"text", request.text},
                               {"recalled", false}};
        if (!save_message(message)) {
            return result(ChatServiceStatus::unavailable, "failed to save message");
        }
        (void)redis_->command("RPUSH", {channel_messages_key(*channel), message_id});
        publish_event(*channel, {{"type", "message"}, {"channel", *channel}, {"message", message}});
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"message", message}});
    }

    ChatServiceResult ChatService::recall(ChatRecallRequest request) const
    {
        if (request.channel.empty() || request.message_id.empty()) {
            return result(ChatServiceStatus::bad_request, "channel and message_id are required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        auto message = load_message(request.message_id);
        if (!message || message->value("channel", std::string{}) != request.channel) {
            return result(ChatServiceStatus::not_found, "message not found");
        }
        (*message)["recalled"] = true;
        (*message)["text"] = "";
        if (!save_message(*message)) {
            return result(ChatServiceStatus::unavailable, "failed to recall message");
        }
        publish_event(request.channel, {{"type", "recall"}, {"channel", request.channel}, {"message_id", request.message_id}});
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"message_id", request.message_id}});
    }

    ChatServiceResult ChatService::messages(ChatMessagesRequest request) const
    {
        const auto channel = resolve_conversation_channel(request);
        if (!channel) {
            return result(ChatServiceStatus::bad_request, "conversation is required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        request.limit = std::clamp(request.limit, 1, 200);
        const auto ids = redis_->command("LRANGE", {channel_messages_key(*channel), std::to_string(-request.limit), "-1"});
        nlohmann::json messages = nlohmann::json::array();
        if (ids && ids->get_type() == yuan::redis::resp_array) {
            const auto *array = dynamic_cast<yuan::redis::ArrayValue *>(ids.get());
            for (const auto &id : array->get_values()) {
                if (auto message = load_message(id->to_string())) {
                    messages.push_back(*message);
                }
            }
        }
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"channel", *channel}, {"messages", messages}});
    }

    ChatServiceResult ChatService::add_friend(ChatFriendRequest request) const
    {
        if (request.user_id.empty() || request.friend_user_id.empty() || request.user_id == request.friend_user_id) {
            return result(ChatServiceStatus::bad_request, "valid user_id and friend_user_id are required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        (void)redis_->command("SADD", {friend_key(request.user_id), request.friend_user_id});
        (void)redis_->command("SADD", {friend_key(request.friend_user_id), request.user_id});
        publish_event("user:" + request.user_id, {{"type", "friend.add"}, {"user_id", request.user_id}, {"friend_user_id", request.friend_user_id}});
        publish_event("user:" + request.friend_user_id, {{"type", "friend.add"}, {"user_id", request.friend_user_id}, {"friend_user_id", request.user_id}});
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"user_id", request.user_id}, {"friend_user_id", request.friend_user_id}});
    }

    ChatServiceResult ChatService::remove_friend(ChatFriendRequest request) const
    {
        if (request.user_id.empty() || request.friend_user_id.empty()) {
            return result(ChatServiceStatus::bad_request, "user_id and friend_user_id are required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        (void)redis_->command("SREM", {friend_key(request.user_id), request.friend_user_id});
        (void)redis_->command("SREM", {friend_key(request.friend_user_id), request.user_id});
        publish_event("user:" + request.user_id, {{"type", "friend.remove"}, {"user_id", request.user_id}, {"friend_user_id", request.friend_user_id}});
        publish_event("user:" + request.friend_user_id, {{"type", "friend.remove"}, {"user_id", request.friend_user_id}, {"friend_user_id", request.user_id}});
        return result(ChatServiceStatus::ok, {}, {{"ok", true}});
    }

    ChatServiceResult ChatService::list_friends(ChatFriendListRequest request) const
    {
        if (request.user_id.empty()) {
            return result(ChatServiceStatus::bad_request, "user_id is required");
        }
        if (!ensure_redis()) {
            return result(ChatServiceStatus::unavailable, "redis unavailable");
        }
        nlohmann::json friends = nlohmann::json::array();
        const auto values = redis_->command("SMEMBERS", {friend_key(request.user_id)});
        if (values && values->get_type() == yuan::redis::resp_array) {
            const auto *array = dynamic_cast<yuan::redis::ArrayValue *>(values.get());
            for (const auto &value : array->get_values()) {
                friends.push_back(value->to_string());
            }
        }
        return result(ChatServiceStatus::ok, {}, {{"ok", true}, {"user_id", request.user_id}, {"friends", friends}});
    }

    std::optional<std::string> ChatService::resolve_conversation_channel(const ChatPublishRequest &request) const
    {
        switch (request.conversation_type) {
            case ChatConversationType::channel:
                return request.channel.empty() ? std::nullopt : std::optional<std::string>{request.channel};
            case ChatConversationType::world:
                return std::string{"world"};
            case ChatConversationType::group:
                return request.group_id.empty() ? std::nullopt : std::optional<std::string>{"group:" + request.group_id};
            case ChatConversationType::private_chat:
                if (request.from_user_id.empty() || request.to_user_id.empty()) {
                    return std::nullopt;
                }
                return private_channel(request.from_user_id, request.to_user_id);
        }
        return std::nullopt;
    }

    std::optional<std::string> ChatService::resolve_conversation_channel(const ChatMessagesRequest &request) const
    {
        switch (request.conversation_type) {
            case ChatConversationType::channel:
                return request.channel.empty() ? std::nullopt : std::optional<std::string>{request.channel};
            case ChatConversationType::world:
                return std::string{"world"};
            case ChatConversationType::group:
                return request.group_id.empty() ? std::nullopt : std::optional<std::string>{"group:" + request.group_id};
            case ChatConversationType::private_chat:
                if (request.user_id.empty() || request.peer_user_id.empty()) {
                    return std::nullopt;
                }
                return private_channel(request.user_id, request.peer_user_id);
        }
        return std::nullopt;
    }

    bool ChatService::ensure_redis() const
    {
        return redis_ && redis_->ensure_connected();
    }

    std::optional<nlohmann::json> ChatService::load_message(const std::string &message_id) const
    {
        const auto value = redis_->get(message_key(message_id));
        if (!value || value->get_type() == yuan::redis::resp_null) {
            return std::nullopt;
        }
        try {
            return nlohmann::json::parse(value->to_string());
        } catch (...) {
            return std::nullopt;
        }
    }

    bool ChatService::save_message(const nlohmann::json &message) const
    {
        const auto message_id = message.value("message_id", std::string{});
        if (message_id.empty()) {
            return false;
        }
        const auto saved = redis_->set(message_key(message_id), message.dump());
        return saved && saved->to_string() == "OK";
    }

    void ChatService::publish_event(const std::string &channel, const nlohmann::json &event) const
    {
        (void)redis_->command("PUBLISH", {pubsub_channel(channel), event.dump()});
    }
}
