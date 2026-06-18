#ifndef YUAN_GAME_SERVER_COMMON_PROTO_MAIL_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_MAIL_PROTO_H

#include "common/codec/binary_codec.h"

#include "yuan/rpc/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::game::server
{
    namespace mail_state
    {
        inline constexpr std::uint32_t unread = 1;
        inline constexpr std::uint32_t read = 2;
        inline constexpr std::uint32_t attachment_claimed = 3;
        inline constexpr std::uint32_t deleted = 4;
    }

    struct SSMailAttachment
    {
        std::string item_type;
        std::uint64_t item_id = 0;
        std::uint64_t amount = 0;
        std::string payload;

        YUAN_GAME_BINARY_FIELDS(item_type, item_id, amount, payload)
    };

    struct SSMailDetail
    {
        std::uint32_t detail_type = 0;
        yuan::rpc::Bytes detail_data;

        YUAN_GAME_BINARY_FIELDS(detail_type, detail_data)
    };

    struct SSMailStateRecord
    {
        std::uint32_t state = mail_state::unread;
        bool attachment_claimed = false;
        std::uint64_t claimed_at_ms = 0;

        YUAN_GAME_BINARY_FIELDS(state, attachment_claimed, claimed_at_ms)
    };

    struct SSMailIdList
    {
        std::vector<std::uint64_t> mail_ids;

        YUAN_GAME_BINARY_FIELDS(mail_ids)
    };
}

#endif
