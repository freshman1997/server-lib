#ifndef YUAN_GAME_SERVER_COMMON_PROTO_RANK_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_RANK_PROTO_H

#include "common/proto/base_proto.h"

namespace yuan::game::server
{
    struct SSRankRoleSummary
    {
        RoleId role_id = 0;
        PlayerUid player_uid = 0;
        std::string name;
        std::uint32_t level = 1;
        PackedGameServiceId world_service_id = 0;

        YUAN_GAME_BINARY_FIELDS(role_id, player_uid, name, level, world_service_id)
    };

    struct SSRankRoleUpdateRequest
    {
        SSRankRoleSummary role;

        YUAN_GAME_BINARY_FIELDS(role)
    };

    struct SSRankRoleGetRequest
    {
        RoleId role_id = 0;

        YUAN_GAME_BINARY_FIELDS(role_id)
    };

    struct SSRankRoleResponse
    {
        bool ok = false;
        std::string message;
        bool has_role = false;
        SSRankRoleSummary role;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_role, role)
    };

    struct SSRankScoreUpdateRequest
    {
        std::string board;
        std::string member;
        std::uint64_t score = 0;
        bool has_role = false;
        SSRankRoleSummary role;

        YUAN_GAME_BINARY_FIELDS(board, member, score, has_role, role)
    };

    struct SSRankScoreRemoveRequest
    {
        std::string board;
        std::string member;

        YUAN_GAME_BINARY_FIELDS(board, member)
    };

    struct SSRankScoreGetRequest
    {
        std::string board;
        std::string member;

        YUAN_GAME_BINARY_FIELDS(board, member)
    };

    struct SSRankTopGetRequest
    {
        std::string board;
        std::uint32_t limit = 10;

        YUAN_GAME_BINARY_FIELDS(board, limit)
    };

    struct SSRankEntry
    {
        std::string member;
        std::uint64_t score = 0;
        std::uint32_t rank = 0;
        bool has_role = false;
        SSRankRoleSummary role;

        YUAN_GAME_BINARY_FIELDS(member, score, rank, has_role, role)
    };

    struct SSRankScoreResponse
    {
        bool ok = false;
        std::string message;
        std::string board;
        std::string member;
        bool has_score = false;
        std::uint64_t score = 0;
        bool has_role = false;
        SSRankRoleSummary role;

        YUAN_GAME_BINARY_FIELDS(ok, message, board, member, has_score, score, has_role, role)
    };

    struct SSRankTopResponse
    {
        bool ok = false;
        std::string message;
        std::string board;
        std::vector<SSRankEntry> entries;

        YUAN_GAME_BINARY_FIELDS(ok, message, board, entries)
    };
}

#endif
