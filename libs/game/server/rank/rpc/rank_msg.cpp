#include "rank/rpc/rank_msg.h"

#include "value/array_value.h"
#include "value/null_value.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>

namespace yuan::game::server
{
    namespace
    {
        std::string rank_key(const std::string &board)
        {
            return "game:rank:" + board;
        }

        std::string role_key(RoleId role_id)
        {
            return "game:rank:role:" + std::to_string(role_id);
        }

        std::optional<RoleId> role_id_from_member(const std::string &member)
        {
            constexpr std::string_view prefix = "role:";
            if (member.rfind(prefix, 0) != 0 || member.size() == prefix.size()) {
                return std::nullopt;
            }
            try {
                return static_cast<RoleId>(std::stoull(member.substr(prefix.size())));
            } catch (...) {
                return std::nullopt;
            }
        }

        nlohmann::json encode_role_json(const SSRankRoleSummary &role)
        {
            return nlohmann::json{{"role_id", role.role_id},
                                  {"player_uid", role.player_uid},
                                  {"name", role.name},
                                  {"level", role.level},
                                  {"world_service_id", role.world_service_id}};
        }

        std::optional<SSRankRoleSummary> decode_role_json(const std::string &text)
        {
            try {
                const auto root = nlohmann::json::parse(text);
                SSRankRoleSummary role;
                role.role_id = root.value("role_id", static_cast<RoleId>(0));
                role.player_uid = root.value("player_uid", static_cast<PlayerUid>(0));
                role.name = root.value("name", std::string{});
                role.level = root.value("level", static_cast<std::uint32_t>(1));
                role.world_service_id = root.value("world_service_id", static_cast<PackedGameServiceId>(0));
                return role;
            } catch (...) {
                return std::nullopt;
            }
        }

        bool ensure_redis(RankMsgContext &context)
        {
            return context.redis && context.redis->ensure_connected();
        }

        bool save_role(RankMsgContext &context, const SSRankRoleSummary &role)
        {
            if (role.role_id == 0 || !ensure_redis(context)) {
                return false;
            }

            const auto result = context.redis->set(role_key(role.role_id), encode_role_json(role).dump());
            return result && result->to_string() == "OK";
        }

        std::optional<SSRankRoleSummary> load_role(RankMsgContext &context, RoleId role_id)
        {
            if (role_id == 0 || !ensure_redis(context)) {
                return std::nullopt;
            }

            const auto value = context.redis->get(role_key(role_id));
            if (!value || value->get_type() == yuan::redis::resp_null) {
                return std::nullopt;
            }

            return decode_role_json(value->to_string());
        }

        void attach_role(RankMsgContext &context, const std::string &member, bool &has_role, SSRankRoleSummary &role)
        {
            has_role = false;
            const auto role_id = role_id_from_member(member);
            if (!role_id) {
                return;
            }

            const auto loaded = load_role(context, *role_id);
            if (!loaded) {
                return;
            }

            role = *loaded;
            has_role = true;
        }

        yuan::rpc::Response binary_response(const yuan::rpc::Message &message, yuan::rpc::RpcStatus status, yuan::rpc::Bytes payload = {}, std::string error = {})
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = status;
            response.payload = std::move(payload);
            response.error = std::move(error);
            return response;
        }

        yuan::rpc::Response handle_rank_role_update(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankRoleUpdateRequest>(message.payload);
            if (!request || request->role.role_id == 0) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank role update");
            }

            const bool ok = save_role(context, request->role);
            SSRankRoleResponse response{ok, ok ? "ok" : "redis unavailable", ok, request->role};
            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);

            return binary_response(message, ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::unavailable, std::move(payload));
        }

        yuan::rpc::Response handle_rank_role_get(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankRoleGetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank role get");
            }

            SSRankRoleResponse response;
            response.ok = true;
            response.message = "ok";
            if (const auto role = load_role(context, request->role_id)) {
                response.has_role = true;
                response.role = *role;
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);
            return binary_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response handle_rank_score_update(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankScoreUpdateRequest>(message.payload);
            if (!request || request->board.empty() || request->member.empty()) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank score update");
            }

            if (request->has_role) {
                (void)save_role(context, request->role);
            }

            if (!ensure_redis(context)) {
                return binary_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            const auto result = context.redis->command("ZADD", {rank_key(request->board), std::to_string(request->score), request->member});
            SSRankScoreResponse response{static_cast<bool>(result), result ? "ok" : "redis unavailable", request->board, request->member, true, request->score};
            attach_role(context, request->member, response.has_role, response.role);
            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);
            return binary_response(message, response.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::unavailable, std::move(payload));
        }

        yuan::rpc::Response handle_rank_score_remove(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankScoreRemoveRequest>(message.payload);
            if (!request || request->board.empty() || request->member.empty()) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank score remove");
            }

            if (!ensure_redis(context)) {
                return binary_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            const auto result = context.redis->command("ZREM", {rank_key(request->board), request->member});
            SSRankScoreResponse response{static_cast<bool>(result), result ? "ok" : "redis unavailable", request->board, request->member};
            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);
            return binary_response(message, response.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::unavailable, std::move(payload));
        }

        yuan::rpc::Response handle_rank_score_get(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankScoreGetRequest>(message.payload);
            if (!request || request->board.empty() || request->member.empty()) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank score get");
            }

            if (!ensure_redis(context)) {
                return binary_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            SSRankScoreResponse response{true, "ok", request->board, request->member};
            const auto result = context.redis->command("ZSCORE", {rank_key(request->board), request->member});
            if (result && result->get_type() != yuan::redis::resp_null) {
                response.has_score = true;
                response.score = static_cast<std::uint64_t>(std::stoull(result->to_string()));
            }

            attach_role(context, request->member, response.has_role, response.role);
            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);
            return binary_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response handle_rank_top_get(RankMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSRankTopGetRequest>(message.payload);
            if (!request || request->board.empty()) {
                return binary_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid rank top get");
            }

            if (!ensure_redis(context)) {
                return binary_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            const auto limit = std::clamp<std::uint32_t>(request->limit == 0 ? 10 : request->limit, 1, 100);
            SSRankTopResponse response;
            response.ok = true;
            response.message = "ok";
            response.board = request->board;
            const auto result = context.redis->command("ZREVRANGE", {rank_key(request->board), "0", std::to_string(limit - 1), "WITHSCORES"});
            if (result && result->get_type() == yuan::redis::resp_array) {
                const auto *array = dynamic_cast<yuan::redis::ArrayValue *>(result.get());
                const auto &values = array->get_values();
                for (std::size_t i = 0; i + 1 < values.size() && response.entries.size() < limit; i += 2) {
                    SSRankEntry entry;
                    entry.member = values[i]->to_string();
                    entry.score = static_cast<std::uint64_t>(std::stoull(values[i + 1]->to_string()));
                    entry.rank = static_cast<std::uint32_t>(response.entries.size() + 1);
                    attach_role(context, entry.member, entry.has_role, entry.role);
                    response.entries.push_back(std::move(entry));
                }
            }
            
            yuan::rpc::Bytes payload;
            (void)encode_binary(response, payload);
            return binary_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }
    }

    bool register_rank_msg(yuan::rpc::Server &server, RankMsgContext &context)
    {
        const bool role_update = server.register_handler(game_route::rank_role_update(), std::bind_front(handle_rank_role_update, std::ref(context)));
        const bool role_get = server.register_handler(game_route::rank_role_get(), std::bind_front(handle_rank_role_get, std::ref(context)));
        const bool score_update = server.register_handler(game_route::rank_score_update(), std::bind_front(handle_rank_score_update, std::ref(context)));
        const bool score_remove = server.register_handler(game_route::rank_score_remove(), std::bind_front(handle_rank_score_remove, std::ref(context)));
        const bool score_get = server.register_handler(game_route::rank_score_get(), std::bind_front(handle_rank_score_get, std::ref(context)));
        const bool top_get = server.register_handler(game_route::rank_top_get(), std::bind_front(handle_rank_top_get, std::ref(context)));

        return role_update && role_get && score_update && score_remove && score_get && top_get;
    }
}
