#ifndef __YUAN_SERVER_NAS_TYPES_H__
#define __YUAN_SERVER_NAS_TYPES_H__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::server::nas
{
    enum class NasPermission : std::uint32_t
    {
        none = 0,
        read = 1u << 0,
        write = 1u << 1,
        remove = 1u << 2,
        admin = 1u << 3,
    };

    inline NasPermission operator|(NasPermission lhs, NasPermission rhs)
    {
        return static_cast<NasPermission>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
    }

    inline NasPermission operator&(NasPermission lhs, NasPermission rhs)
    {
        return static_cast<NasPermission>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
    }

    inline bool has_permission(NasPermission mask, NasPermission required)
    {
        return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(required)) == static_cast<std::uint32_t>(required);
    }

    struct NasRedisConfig
    {
        bool enabled = true;
        std::string host = "127.0.0.1";
        int port = 6379;
        std::string password;
        int db = 0;
        std::string key_prefix = "yuan:nas:";
        std::size_t audit_max_events = 1000;
    };

    struct NasUser
    {
        std::string id;
        std::string username;
        std::string password_hash;
        bool enabled = true;
        bool admin = false;
    };

    struct NasShare
    {
        std::string id;
        std::string name;
        std::string root_path;
        bool enabled = true;
        bool readonly = false;
        NasPermission default_permissions = NasPermission::read;
        std::unordered_map<std::string, NasPermission> subject_permissions;
    };

    struct NasWebDavLockRecord
    {
        std::string token;
        std::string share_id;
        std::string path;
        std::string scope = "exclusive";
        std::string depth = "infinity";
        std::string owner;
        std::int64_t expires_at_unix_ms = 0;
    };

    struct NasAuditEvent
    {
        std::int64_t timestamp_unix_ms = 0;
        std::string actor;
        std::string action;
        std::string target;
        std::string detail;
    };

    struct NasAuditConfig
    {
        bool file_enabled = true;
        std::string file_path = "nas_audit.jsonl";
        std::size_t max_events = 1000;
    };

    struct NasAdminSession
    {
        std::string id;
        std::string username;
        std::string remote_addr;
        std::string user_agent;
        std::string last_path;
        std::int64_t created_at_unix_ms = 0;
        std::int64_t last_seen_unix_ms = 0;
        std::uint64_t request_count = 0;
    };

    struct NasConfig
    {
        NasRedisConfig redis;
        NasAuditConfig audit;
        std::string webdav_mount = "/dav";
        std::string admin_console_path;
        bool allow_anonymous_read = false;
        std::vector<NasShare> shares;
    };
}

#endif
