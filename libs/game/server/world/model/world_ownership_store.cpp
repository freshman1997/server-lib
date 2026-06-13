#include "world/model/world_ownership_store.h"

#include "internal/def.h"

#include <charconv>
#include <string_view>

namespace yuan::game::server
{
    namespace
    {
        std::optional<std::uint64_t> parse_u64(std::string_view text)
        {
            std::uint64_t value = 0;
            const auto *begin = text.data();
            const auto *end = begin + text.size();
            const auto result = std::from_chars(begin, end, value);
            if (result.ec != std::errc{} || result.ptr != end) {
                return std::nullopt;
            }
            return value;
        }

        std::string encode_record(WorldOwnershipRecord record)
        {
            return std::to_string(record.zone_service_id) + ":" + std::to_string(record.gateway_session_id);
        }

        std::optional<WorldOwnershipRecord> decode_record(std::string_view value)
        {
            const auto colon = value.find(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            const auto zone = parse_u64(value.substr(0, colon));
            const auto session = parse_u64(value.substr(colon + 1));
            if (!zone || !session) {
                return std::nullopt;
            }
            return WorldOwnershipRecord{*zone, *session};
        }
    }

    std::optional<WorldOwnershipRecord> InMemoryWorldOwnershipStore::get(PlayerId player_id) const
    {
        const auto it = records_.find(player_id);
        if (it == records_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    bool InMemoryWorldOwnershipStore::compare_and_set(PlayerId player_id,
                                                       PackedGameServiceId source_zone_service_id,
                                                       std::uint64_t expected_gateway_session_id,
                                                       WorldOwnershipRecord next)
    {
        const auto current = get(player_id).value_or(WorldOwnershipRecord{});
        if (next.zone_service_id == 0 && source_zone_service_id != 0 && current.zone_service_id != 0 && current.zone_service_id != source_zone_service_id) {
            return false;
        }
        if (next.zone_service_id == 0 && expected_gateway_session_id != 0 && current.gateway_session_id != 0 && current.gateway_session_id != expected_gateway_session_id) {
            return false;
        }
        if (next.zone_service_id == 0) {
            records_.erase(player_id);
        } else {
            records_[player_id] = next;
        }
        return true;
    }

    RedisWorldOwnershipStore::RedisWorldOwnershipStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix)
        : redis_(std::move(redis)), key_prefix_(std::move(key_prefix))
    {
    }

    std::optional<WorldOwnershipRecord> RedisWorldOwnershipStore::get(PlayerId player_id) const
    {
        if (!redis_) {
            return std::nullopt;
        }
        const auto value = redis_->get(key(player_id));
        if (!value || value->get_type() == yuan::redis::resp_null || value->get_type() == yuan::redis::resp_error) {
            return std::nullopt;
        }
        return decode_record(value->to_string());
    }

    bool RedisWorldOwnershipStore::compare_and_set(PlayerId player_id,
                                                   PackedGameServiceId source_zone_service_id,
                                                   std::uint64_t expected_gateway_session_id,
                                                   WorldOwnershipRecord next)
    {
        if (!redis_) {
            return false;
        }
        static const std::string script =
            "local cur = redis.call('GET', KEYS[1]) "
            "local cur_zone = 0 "
            "local cur_session = 0 "
            "if cur then "
            "  local sep = string.find(cur, ':') "
            "  if sep then "
            "    cur_zone = tonumber(string.sub(cur, 1, sep - 1)) or 0 "
            "    cur_session = tonumber(string.sub(cur, sep + 1)) or 0 "
            "  end "
            "end "
            "local next_zone = tonumber(ARGV[1]) or 0 "
            "local next_session = tonumber(ARGV[2]) or 0 "
            "local source_zone = tonumber(ARGV[3]) or 0 "
            "local expected_session = tonumber(ARGV[4]) or 0 "
            "if next_zone == 0 and source_zone ~= 0 and cur_zone ~= 0 and cur_zone ~= source_zone then return 0 end "
            "if next_zone == 0 and expected_session ~= 0 and cur_session ~= 0 and cur_session ~= expected_session then return 0 end "
            "if next_zone == 0 then redis.call('DEL', KEYS[1]) else redis.call('SET', KEYS[1], ARGV[1] .. ':' .. ARGV[2]) end "
            "return 1";
        const auto result = redis_->eval(script,
                                         {key(player_id)},
                                         {std::to_string(next.zone_service_id),
                                          std::to_string(next.gateway_session_id),
                                          std::to_string(source_zone_service_id),
                                          std::to_string(expected_gateway_session_id)});
        return result && result->get_type() != yuan::redis::resp_error && result->to_string() == "1";
    }

    std::string RedisWorldOwnershipStore::key(PlayerId player_id) const
    {
        return key_prefix_ + std::to_string(player_id);
    }
}
