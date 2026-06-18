#include "chat_web/handler/chat_handler.h"

#include "http_server.h"
#include "request.h"
#include "response.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace yuan::game::server
{
    namespace
    {
        std::string error_json(const std::string &message)
        {
            return nlohmann::json{{"ok", false}, {"message", message}}.dump();
        }

        std::string body_text(yuan::net::http::HttpRequest *request)
        {
            const auto *body = request->body_begin();
            return body ? std::string(body, request->get_body_length()) : std::string{};
        }

        yuan::net::http::ResponseCode response_code(ChatServiceStatus status)
        {
            switch (status) {
                case ChatServiceStatus::ok:
                    return yuan::net::http::ResponseCode::ok_;
                case ChatServiceStatus::bad_request:
                    return yuan::net::http::ResponseCode::bad_request;
                case ChatServiceStatus::not_found:
                    return yuan::net::http::ResponseCode::not_found;
                case ChatServiceStatus::unavailable:
                    return yuan::net::http::ResponseCode::service_unavailable;
            }
            return yuan::net::http::ResponseCode::internal_server_error;
        }

        void write_result(yuan::net::http::HttpResponse *response, const ChatServiceResult &result)
        {
            response->json(result.body.dump(), response_code(result.status));
        }

        std::optional<nlohmann::json> parse_body(yuan::net::http::HttpRequest *request)
        {
            try {
                return nlohmann::json::parse(body_text(request));
            } catch (...) {
                return std::nullopt;
            }
        }

        ChatPublishRequest publish_request(const nlohmann::json &root, ChatConversationType type)
        {
            ChatPublishRequest request;
            request.conversation_type = type;
            request.channel = root.value("channel", std::string{});
            request.group_id = root.value("group_id", std::string{});
            request.from_user_id = root.value("from_user_id", root.value("user_id", std::string{}));
            request.to_user_id = root.value("to_user_id", std::string{});
            request.text = root.value("text", std::string{});
            request.message_type = root.value("message_type", std::string{"text"});
            return request;
        }

        ChatMessagesRequest messages_request(yuan::net::http::HttpRequest *request, ChatConversationType type, int limit)
        {
            ChatMessagesRequest chat_request;
            chat_request.conversation_type = type;
            chat_request.channel = request->get_param("channel");
            chat_request.group_id = request->get_param("group_id");
            chat_request.user_id = request->get_param("user_id");
            chat_request.peer_user_id = request->get_param("peer_user_id");
            chat_request.limit = limit;
            return chat_request;
        }

        void register_publish_route(yuan::net::http::HttpServer &server, ChatHandlerContext &context, const std::string &path, ChatConversationType type)
        {
            server.on(path, [&context, type](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
                if (!request->is_post()) {
                    response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                    return;
                }
                const auto root = parse_body(request);
                if (!root) {
                    response->json(error_json("invalid publish request"), yuan::net::http::ResponseCode::bad_request);
                    return;
                }
                if (!context.publish_handler) {
                    response->json(error_json("publish handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                    return;
                }
                write_result(response, context.publish_handler(publish_request(*root, type)));
            });
        }

        void register_messages_route(yuan::net::http::HttpServer &server, ChatHandlerContext &context, const std::string &path, ChatConversationType type)
        {
            server.on(path, [&context, type](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
                if (!request->is_get()) {
                    response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                    return;
                }
                if (!context.messages_handler) {
                    response->json(error_json("messages handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                    return;
                }
                int limit = 50;
                const auto limit_text = request->get_param("limit");
                if (!limit_text.empty()) {
                    try {
                        limit = std::stoi(limit_text);
                    } catch (...) {
                        response->json(error_json("invalid limit"), yuan::net::http::ResponseCode::bad_request);
                        return;
                    }
                }
                write_result(response, context.messages_handler(messages_request(request, type, std::clamp(limit, 1, 200))));
            });
        }

        std::optional<ChatFriendRequest> parse_friend_request(yuan::net::http::HttpRequest *request)
        {
            const auto root = parse_body(request);
            if (!root) {
                return std::nullopt;
            }
            return ChatFriendRequest{root->value("user_id", std::string{}), root->value("friend_user_id", std::string{})};
        }
    }

    bool register_chat_http_handlers(yuan::net::http::HttpServer &server, ChatHandlerContext &context)
    {
        server.on("/chat/subscribe", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto root = parse_body(request);
            if (!root) {
                response->json(error_json("invalid subscribe request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.subscribe_handler) {
                response->json(error_json("subscribe handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.subscribe_handler(ChatSubscriptionRequest{root->value("channel", std::string{}), root->value("user_id", std::string{})}));
        });

        server.on("/chat/unsubscribe", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto root = parse_body(request);
            if (!root) {
                response->json(error_json("invalid unsubscribe request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.unsubscribe_handler) {
                response->json(error_json("unsubscribe handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.unsubscribe_handler(ChatSubscriptionRequest{root->value("channel", std::string{}), root->value("user_id", std::string{})}));
        });

        register_publish_route(server, context, "/chat/publish", ChatConversationType::channel);
        register_publish_route(server, context, "/chat/world/publish", ChatConversationType::world);
        register_publish_route(server, context, "/chat/group/publish", ChatConversationType::group);
        register_publish_route(server, context, "/chat/private/publish", ChatConversationType::private_chat);

        server.on("/chat/recall", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto root = parse_body(request);
            if (!root) {
                response->json(error_json("invalid recall request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.recall_handler) {
                response->json(error_json("recall handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.recall_handler(ChatRecallRequest{root->value("channel", std::string{}), root->value("message_id", std::string{})}));
        });

        register_messages_route(server, context, "/chat/messages", ChatConversationType::channel);
        register_messages_route(server, context, "/chat/world/messages", ChatConversationType::world);
        register_messages_route(server, context, "/chat/group/messages", ChatConversationType::group);
        register_messages_route(server, context, "/chat/private/messages", ChatConversationType::private_chat);

        server.on("/chat/friend/add", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto friend_request = parse_friend_request(request);
            if (!friend_request) {
                response->json(error_json("invalid friend add request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.add_friend_handler) {
                response->json(error_json("friend add handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.add_friend_handler(*friend_request));
        });

        server.on("/chat/friend/remove", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_post()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            const auto friend_request = parse_friend_request(request);
            if (!friend_request) {
                response->json(error_json("invalid friend remove request"), yuan::net::http::ResponseCode::bad_request);
                return;
            }
            if (!context.remove_friend_handler) {
                response->json(error_json("friend remove handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.remove_friend_handler(*friend_request));
        });

        server.on("/chat/friend/list", [&context](yuan::net::http::HttpRequest *request, yuan::net::http::HttpResponse *response) {
            if (!request->is_get()) {
                response->json(error_json("method not allowed"), yuan::net::http::ResponseCode::method_not_allowed);
                return;
            }
            if (!context.list_friends_handler) {
                response->json(error_json("friend list handler is not configured"), yuan::net::http::ResponseCode::internal_server_error);
                return;
            }
            write_result(response, context.list_friends_handler(ChatFriendListRequest{request->get_param("user_id")}));
        });

        return true;
    }
}
