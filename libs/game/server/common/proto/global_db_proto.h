#ifndef YUAN_GAME_SERVER_COMMON_PROTO_GLOBAL_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_GLOBAL_DB_PROTO_H

#include "common/codec/binary_codec.h"
#include "common/proto/base_proto.h"
#include "common/proto/mail_proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::game::server
{
    struct SSGlobalDbConfigGetRequest
    {
        std::string key;

        YUAN_GAME_BINARY_FIELDS(key)
    };

    struct SSGlobalDbConfigSetRequest
    {
        std::string key;
        std::string value;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(key, value, data_version)
    };

    struct SSGlobalDbConfigResponse
    {
        bool ok = false;
        std::string message;
        bool has_value = false;
        std::string key;
        std::string value;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_value, key, value, data_version)
    };

    namespace global_db_mail_scope
    {
        inline constexpr std::uint32_t player = 1;
        inline constexpr std::uint32_t global = 2;
        inline constexpr std::uint32_t operator_ = 3;
    }

    struct SSGlobalDbMailRecord
    {
        std::uint64_t mail_id = 0;
        std::uint32_t scope = global_db_mail_scope::player;
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string title;
        std::string body;
        SSMailDetail detail;
        std::string sender;
        std::string operator_id;
        std::string operator_reason;
        std::vector<SSMailAttachment> attachments;
        std::uint64_t created_at_ms = 0;
        std::uint64_t starts_at_ms = 0;
        std::uint64_t expires_at_ms = 0;
        std::string dedupe_key;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(mail_id,
                                scope,
                                player_uid,
                                role_id,
                                title,
                                body,
                                detail,
                                sender,
                                operator_id,
                                operator_reason,
                                attachments,
                                created_at_ms,
                                starts_at_ms,
                                expires_at_ms,
                                dedupe_key,
                                data_version)
    };

    struct SSGlobalDbMailBoxItem
    {
        SSGlobalDbMailRecord mail;
        std::uint32_t state = mail_state::unread;
        bool attachment_claimed = false;
        std::uint64_t claimed_at_ms = 0;

        YUAN_GAME_BINARY_FIELDS(mail, state, attachment_claimed, claimed_at_ms)
    };

    struct SSGlobalDbMailCreateRequest
    {
        std::uint32_t scope = global_db_mail_scope::player;
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string title;
        std::string body;
        SSMailDetail detail;
        std::string sender;
        std::string operator_id;
        std::string operator_reason;
        std::vector<SSMailAttachment> attachments;
        std::uint64_t now_ms = 0;
        std::uint64_t starts_at_ms = 0;
        std::uint64_t expires_at_ms = 0;
        std::string dedupe_key;

        YUAN_GAME_BINARY_FIELDS(scope,
                                player_uid,
                                role_id,
                                title,
                                body,
                                detail,
                                sender,
                                operator_id,
                                operator_reason,
                                attachments,
                                now_ms,
                                starts_at_ms,
                                expires_at_ms,
                                dedupe_key)
    };

    struct SSGlobalDbMailCreateResponse
    {
        bool ok = false;
        std::string message;
        std::uint64_t mail_id = 0;
        bool duplicated = false;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, mail_id, duplicated, data_version)
    };

    struct SSGlobalDbMailListRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        bool include_global = true;
        bool include_operator = true;
        std::uint32_t limit = 50;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, include_global, include_operator, limit, now_ms)
    };

    struct SSGlobalDbMailListResponse
    {
        bool ok = false;
        std::string message;
        std::vector<SSGlobalDbMailBoxItem> mails;

        YUAN_GAME_BINARY_FIELDS(ok, message, mails)
    };

    struct SSGlobalDbMailGetRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t mail_id = 0;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, mail_id, now_ms)
    };

    struct SSGlobalDbMailGetResponse
    {
        bool ok = false;
        std::string message;
        bool has_mail = false;
        SSGlobalDbMailBoxItem mail;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_mail, mail)
    };

    struct SSGlobalDbMailClaimAttachmentRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t mail_id = 0;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, mail_id, now_ms)
    };

    struct SSGlobalDbMailClaimAttachmentResponse
    {
        bool ok = false;
        std::string message;
        bool claimed = false;
        std::uint64_t mail_id = 0;
        std::vector<SSMailAttachment> attachments;
        std::uint64_t claimed_at_ms = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, claimed, mail_id, attachments, claimed_at_ms)
    };
}

#endif
