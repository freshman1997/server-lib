#ifndef YUAN_GAME_SERVER_COMMON_PROTO_PLAYER_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_PLAYER_DB_PROTO_H

#include "common/codec/binary_codec.h"
#include "common/proto/base_proto.h"
#include "common/proto/mail_proto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::game::server
{
    struct SSPlayerRoleData
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint32_t level = 1;
        std::uint64_t exp = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, level, exp)
    };

    struct SSPlayerDbLoadRoleRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id)
    };

    struct SSPlayerDbSaveRoleRequest
    {
        SSPlayerRoleData role;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(role, data_version)
    };

    struct SSPlayerDbCreateRoleRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string name;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, name)
    };

    struct SSPlayerDbRoleResponse
    {
        bool ok = false;
        std::string message;
        bool has_role = false;
        SSPlayerRoleData role;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_role, role, data_version)
    };

    struct SSPlayerDbMailRecord
    {
        std::uint64_t mail_id = 0;
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string title;
        std::string body;
        SSMailDetail detail;
        std::string sender;
        std::vector<SSMailAttachment> attachments;
        std::uint64_t created_at_ms = 0;
        std::uint64_t starts_at_ms = 0;
        std::uint64_t expires_at_ms = 0;
        std::string dedupe_key;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(mail_id,
                                player_uid,
                                role_id,
                                title,
                                body,
                                detail,
                                sender,
                                attachments,
                                created_at_ms,
                                starts_at_ms,
                                expires_at_ms,
                                dedupe_key,
                                data_version)
    };

    struct SSPlayerDbMailBoxItem
    {
        SSPlayerDbMailRecord mail;
        std::uint32_t state = mail_state::unread;
        bool attachment_claimed = false;
        std::uint64_t claimed_at_ms = 0;

        YUAN_GAME_BINARY_FIELDS(mail, state, attachment_claimed, claimed_at_ms)
    };

    struct SSPlayerDbMailCreateRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::string title;
        std::string body;
        SSMailDetail detail;
        std::string sender;
        std::vector<SSMailAttachment> attachments;
        std::uint64_t now_ms = 0;
        std::uint64_t starts_at_ms = 0;
        std::uint64_t expires_at_ms = 0;
        std::string dedupe_key;

        YUAN_GAME_BINARY_FIELDS(player_uid,
                                role_id,
                                title,
                                body,
                                detail,
                                sender,
                                attachments,
                                now_ms,
                                starts_at_ms,
                                expires_at_ms,
                                dedupe_key)
    };

    struct SSPlayerDbMailCreateResponse
    {
        bool ok = false;
        std::string message;
        std::uint64_t mail_id = 0;
        bool duplicated = false;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, mail_id, duplicated, data_version)
    };

    struct SSPlayerDbMailListRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint32_t limit = 50;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, limit, now_ms)
    };

    struct SSPlayerDbMailListResponse
    {
        bool ok = false;
        std::string message;
        std::vector<SSPlayerDbMailBoxItem> mails;

        YUAN_GAME_BINARY_FIELDS(ok, message, mails)
    };

    struct SSPlayerDbMailGetRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t mail_id = 0;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, mail_id, now_ms)
    };

    struct SSPlayerDbMailGetResponse
    {
        bool ok = false;
        std::string message;
        bool has_mail = false;
        SSPlayerDbMailBoxItem mail;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_mail, mail)
    };

    struct SSPlayerDbMailClaimAttachmentRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t mail_id = 0;
        std::uint64_t now_ms = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, mail_id, now_ms)
    };

    struct SSPlayerDbMailClaimAttachmentResponse
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
