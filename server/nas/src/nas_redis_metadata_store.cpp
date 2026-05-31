#include "nas/nas_redis_metadata_store.h"

#include "option.h"
#include "redis_client.h"
#include "redis_value.h"
#include "value/array_value.h"
#include "value/int_value.h"
#include "value/map_value.h"
#include "value/status_value.h"
#include "value/string_value.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <utility>

namespace yuan::server::nas
{
    namespace
    {
        bool redis_error(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            return !value || value->get_type() == yuan::redis::resp_error;
        }

        bool redis_ok(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            if (redis_error(value)) {
                return false;
            }
            if (auto status = value->as<yuan::redis::StatusValue>()) {
                return status->is_ok();
            }
            return true;
        }

        std::optional<std::string> redis_string(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            if (redis_error(value) || value->get_type() == 0) {
                return std::nullopt;
            }
            if (auto str = value->as<yuan::redis::StringValue>()) {
                return str->get_value();
            }
            return value->to_string();
        }

        std::unordered_map<std::string, std::string> redis_hash(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            std::unordered_map<std::string, std::string> out;
            if (redis_error(value)) {
                return out;
            }
            if (auto map = value->as<yuan::redis::MapValue>()) {
                for (const auto &[k, v] : map->get_map_value()) {
                    out[k] = v ? v->to_string() : "";
                }
                return out;
            }
            if (auto arr = value->as<yuan::redis::ArrayValue>()) {
                const auto &values = arr->get_values();
                for (std::size_t i = 0; i + 1 < values.size(); i += 2) {
                    out[values[i] ? values[i]->to_string() : ""] = values[i + 1] ? values[i + 1]->to_string() : "";
                }
            }
            return out;
        }

        std::vector<std::string> redis_string_array(const std::shared_ptr<yuan::redis::RedisValue> &value)
        {
            std::vector<std::string> out;
            if (redis_error(value)) {
                return out;
            }
            if (auto arr = value->as<yuan::redis::ArrayValue>()) {
                for (const auto &item : arr->get_values()) {
                    if (item) {
                        out.push_back(item->to_string());
                    }
                }
            }
            return out;
        }

        std::string audit_to_json(const NasAuditEvent &event)
        {
            nlohmann::json j;
            j["timestamp_unix_ms"] = event.timestamp_unix_ms;
            j["actor"] = event.actor;
            j["action"] = event.action;
            j["target"] = event.target;
            j["detail"] = event.detail;
            return j.dump();
        }

        std::optional<NasAuditEvent> audit_from_json(const std::string &text)
        {
            try {
                auto j = nlohmann::json::parse(text);
                NasAuditEvent event;
                event.timestamp_unix_ms = j.value("timestamp_unix_ms", 0ll);
                event.actor = j.value("actor", "");
                event.action = j.value("action", "");
                event.target = j.value("target", "");
                event.detail = j.value("detail", "");
                return event;
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    NasRedisMetadataStore::NasRedisMetadataStore(NasRedisConfig config)
        : config_(std::move(config))
    {
    }

    NasRedisMetadataStore::NasRedisMetadataStore(NasRedisConfig config, std::shared_ptr<yuan::redis::RedisClient> client)
        : config_(std::move(config)), client_(std::move(client))
    {
    }

    bool NasRedisMetadataStore::init()
    {
        if (client_ && client_->is_connected()) {
            return true;
        }

        yuan::redis::Option opt;
        opt.host_ = config_.host;
        opt.port_ = config_.port;
        opt.password_ = config_.password;
        opt.db_ = config_.db;
        opt.name_ = "nas_metadata";

        client_ = std::make_shared<yuan::redis::RedisClient>(opt);
        auto ping = client_->ping();
        return !redis_error(ping) && client_->is_connected();
    }

    bool NasRedisMetadataStore::available() const
    {
        return client_ && client_->is_connected();
    }

    std::optional<NasUser> NasRedisMetadataStore::find_user_by_name(std::string_view username) const
    {
        if (!available()) {
            return std::nullopt;
        }
        auto user_id = redis_string(client_->get(user_by_name_key(username)));
        return user_id ? load_user_by_id(*user_id) : std::nullopt;
    }

    std::vector<NasUser> NasRedisMetadataStore::list_users() const
    {
        std::vector<NasUser> out;
        if (!available()) {
            return out;
        }
        for (const auto &id : redis_string_array(client_->smembers(key("users")))) {
            if (auto user = load_user_by_id(id)) {
                out.push_back(std::move(*user));
            }
        }
        return out;
    }

    bool NasRedisMetadataStore::upsert_user(const NasUser &user)
    {
        if (!available() || user.id.empty() || user.username.empty()) {
            return false;
        }
        const std::unordered_map<std::string, std::string> fields{
            { "id", user.id },
            { "username", user.username },
            { "password_hash", user.password_hash },
            { "smb_password_hash", user.smb_password_hash },
            { "enabled", user.enabled ? "1" : "0" },
            { "admin", user.admin ? "1" : "0" },
        };
        bool ok = true;
        const auto ukey = user_key(user.id);
        for (const auto &[field, value] : fields) {
            ok = !redis_error(client_->hset(ukey, field, value)) && ok;
        }
        return ok && redis_ok(client_->set(user_by_name_key(user.username), user.id)) &&
               !redis_error(client_->sadd(key("users"), { user.id }));
    }

    std::optional<NasShare> NasRedisMetadataStore::find_share_by_name(std::string_view share_name) const
    {
        if (!available()) {
            return std::nullopt;
        }
        auto share_id = redis_string(client_->get(share_by_name_key(share_name)));
        return share_id ? load_share_by_id(*share_id) : std::nullopt;
    }

    std::vector<NasShare> NasRedisMetadataStore::list_shares() const
    {
        std::vector<NasShare> out;
        if (!available()) {
            return out;
        }
        for (const auto &id : redis_string_array(client_->smembers(key("shares")))) {
            if (auto share = load_share_by_id(id)) {
                out.push_back(std::move(*share));
            }
        }
        return out;
    }

    bool NasRedisMetadataStore::upsert_share(const NasShare &share)
    {
        if (!available() || share.id.empty() || share.name.empty()) {
            return false;
        }
        const std::unordered_map<std::string, std::string> fields{
            { "id", share.id },
            { "name", share.name },
            { "root_path", share.root_path },
            { "enabled", share.enabled ? "1" : "0" },
            { "readonly", share.readonly ? "1" : "0" },
            { "default_permissions", permission_to_string(share.default_permissions) },
        };
        bool ok = true;
        const auto skey = share_key(share.id);
        for (const auto &[field, value] : fields) {
            ok = !redis_error(client_->hset(skey, field, value)) && ok;
        }
        ok = ok &&
                  redis_ok(client_->set(share_by_name_key(share.name), share.id)) &&
                  !redis_error(client_->sadd(key("shares"), { share.id }));
        for (const auto &[subject, permissions] : share.subject_permissions) {
            ok = set_permissions(share.id, subject, permissions) && ok;
        }
        return ok;
    }

    NasPermission NasRedisMetadataStore::permissions_for(std::string_view share_id, std::string_view subject) const
    {
        if (!available()) {
            return NasPermission::none;
        }
        auto value = redis_string(client_->hget(acl_key(share_id), std::string(subject)));
        return value ? permission_from_string(*value) : NasPermission::none;
    }

    bool NasRedisMetadataStore::set_permissions(std::string_view share_id, std::string_view subject, NasPermission permissions)
    {
        if (!available() || share_id.empty() || subject.empty()) {
            return false;
        }
        return !redis_error(client_->hset(acl_key(share_id), std::string(subject), permission_to_string(permissions)));
    }

    bool NasRedisMetadataStore::remove_permissions(std::string_view share_id, std::string_view subject)
    {
        if (!available() || share_id.empty() || subject.empty()) {
            return false;
        }
        return !redis_error(client_->hdel(acl_key(share_id), { std::string(subject) }));
    }

    std::unordered_map<std::string, std::string> NasRedisMetadataStore::dead_properties(std::string_view share_id,
                                                                                       std::string_view path) const
    {
        if (!available()) {
            return {};
        }
        return redis_hash(client_->hgetall(dead_property_key(share_id, path)));
    }

    bool NasRedisMetadataStore::set_dead_property(std::string_view share_id,
                                                  std::string_view path,
                                                  std::string_view name,
                                                  std::string_view value)
    {
        if (!available() || share_id.empty() || name.empty()) {
            return false;
        }
        return !redis_error(client_->hset(dead_property_key(share_id, path), std::string(name), std::string(value)));
    }

    bool NasRedisMetadataStore::remove_dead_property(std::string_view share_id,
                                                     std::string_view path,
                                                     std::string_view name)
    {
        if (!available() || share_id.empty() || name.empty()) {
            return false;
        }
        return !redis_error(client_->hdel(dead_property_key(share_id, path), { std::string(name) }));
    }

    bool NasRedisMetadataStore::upsert_webdav_lock(const NasWebDavLockRecord &lock)
    {
        if (!available() || lock.token.empty() || lock.share_id.empty()) {
            return false;
        }
        const std::unordered_map<std::string, std::string> fields{
            { "token", lock.token },
            { "share_id", lock.share_id },
            { "path", std::string(lock.path) },
            { "scope", lock.scope },
            { "depth", lock.depth },
            { "owner", lock.owner },
            { "expires_at_unix_ms", std::to_string(lock.expires_at_unix_ms) },
        };
        bool ok = true;
        const auto lkey = lock_key(lock.token);
        for (const auto &[field, value] : fields) {
            ok = !redis_error(client_->hset(lkey, field, value)) && ok;
        }
        return ok && !redis_error(client_->sadd(lock_index_key(lock.share_id), { lock.token }));
    }

    bool NasRedisMetadataStore::try_create_webdav_lock(const NasWebDavLockRecord &lock)
    {
        if (!available() || lock.token.empty() || lock.share_id.empty()) {
            return false;
        }

        auto set_res = client_->set(lock_guard_key(lock.token), "1", 60, 1);
        auto set_text = redis_string(set_res);
        if (!set_text || *set_text != "OK") {
            return false;
        }

        const std::unordered_map<std::string, std::string> fields{
            { "token", lock.token },
            { "share_id", lock.share_id },
            { "path", std::string(lock.path) },
            { "scope", lock.scope },
            { "depth", lock.depth },
            { "owner", lock.owner },
            { "expires_at_unix_ms", std::to_string(lock.expires_at_unix_ms) },
        };
        bool ok = true;
        const auto lkey = lock_key(lock.token);
        for (const auto &[field, value] : fields) {
            ok = !redis_error(client_->hset(lkey, field, value)) && ok;
        }
        ok = ok && !redis_error(client_->sadd(lock_index_key(lock.share_id), { lock.token }));
        if (!ok) {
            (void)client_->del({ lock_guard_key(lock.token) });
            (void)client_->del({ lkey });
            (void)client_->srem(lock_index_key(lock.share_id), { lock.token });
        }
        return ok;
    }

    std::optional<NasWebDavLockRecord> NasRedisMetadataStore::find_webdav_lock(std::string_view token) const
    {
        if (!available()) {
            return std::nullopt;
        }
        return load_lock_by_token(token);
    }

    std::vector<NasWebDavLockRecord> NasRedisMetadataStore::list_webdav_locks(std::string_view share_id,
                                                                              std::string_view path) const
    {
        std::vector<NasWebDavLockRecord> out;
        if (!available()) {
            return out;
        }
        const std::string target = normalize_path_key(path);
        for (const auto &token : redis_string_array(client_->smembers(lock_index_key(share_id)))) {
            auto lock = load_lock_by_token(token);
            if (!lock) {
                continue;
            }
            const std::string lock_path = normalize_path_key(lock->path);
            const bool exact = lock_path == target;
            const bool child = lock->depth == "infinity" &&
                               (target.rfind(lock_path + "/", 0) == 0 || lock_path == ".");
            if (exact || child) {
                out.push_back(std::move(*lock));
            }
        }
        return out;
    }

    bool NasRedisMetadataStore::remove_webdav_lock(std::string_view token)
    {
        if (!available() || token.empty()) {
            return false;
        }
        auto lock = load_lock_by_token(token);
        if (lock) {
            (void)client_->srem(lock_index_key(lock->share_id), { std::string(token) });
        }
        return !redis_error(client_->del({ lock_key(token) }));
    }

    std::size_t NasRedisMetadataStore::prune_expired_webdav_locks()
    {
        if (!available()) {
            return 0;
        }

        std::size_t removed = 0;
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto &share_id : redis_string_array(client_->smembers(key("shares")))) {
            for (const auto &token : redis_string_array(client_->smembers(lock_index_key(share_id)))) {
                auto lock = load_lock_by_token(token);
                if (!lock) {
                    (void)client_->srem(lock_index_key(share_id), { token });
                    continue;
                }
                if (lock->expires_at_unix_ms > 0 && lock->expires_at_unix_ms <= now_ms) {
                    if (remove_webdav_lock(lock->token)) {
                        ++removed;
                    }
                }
            }
        }

        return removed;
    }

    bool NasRedisMetadataStore::append_audit_event(const NasAuditEvent &event)
    {
        if (!available() || event.action.empty()) {
            return false;
        }
        const auto audit_key = key("audit");
        if (redis_error(client_->rpush(audit_key, { audit_to_json(event) }))) {
            return false;
        }
        const auto cap = config_.audit_max_events > 0
            ? static_cast<std::int64_t>(config_.audit_max_events)
            : static_cast<std::int64_t>(1000);
        (void)client_->ltrim(audit_key, -cap, -1);
        return true;
    }

    std::vector<NasAuditEvent> NasRedisMetadataStore::list_audit_events(std::size_t limit) const
    {
        std::vector<NasAuditEvent> out;
        if (!available() || limit == 0) {
            return out;
        }
        const auto start = -static_cast<std::int64_t>(limit);
        for (const auto &text : redis_string_array(client_->lrange(key("audit"), start, -1))) {
            if (auto event = audit_from_json(text)) {
                out.push_back(std::move(*event));
            }
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    bool NasRedisMetadataStore::upsert_admin_session(const NasAdminSession &session)
    {
        if (!available() || session.id.empty() || session.username.empty()) {
            return false;
        }
        const std::unordered_map<std::string, std::string> fields{
            { "id", session.id },
            { "username", session.username },
            { "remote_addr", session.remote_addr },
            { "user_agent", session.user_agent },
            { "last_path", session.last_path },
            { "created_at_unix_ms", std::to_string(session.created_at_unix_ms) },
            { "last_seen_unix_ms", std::to_string(session.last_seen_unix_ms) },
            { "request_count", std::to_string(session.request_count) },
        };
        bool ok = true;
        const auto skey = admin_session_key(session.id);
        for (const auto &[field, value] : fields) {
            ok = !redis_error(client_->hset(skey, field, value)) && ok;
        }
        return ok && !redis_error(client_->sadd(key("admin:sessions"), { session.id }));
    }

    std::vector<NasAdminSession> NasRedisMetadataStore::list_admin_sessions(std::size_t limit) const
    {
        std::vector<NasAdminSession> out;
        if (!available() || limit == 0) {
            return out;
        }
        for (const auto &id : redis_string_array(client_->smembers(key("admin:sessions")))) {
            if (auto session = load_admin_session_by_id(id)) {
                out.push_back(std::move(*session));
            }
        }
        std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.last_seen_unix_ms > rhs.last_seen_unix_ms;
        });
        if (out.size() > limit) {
            out.resize(limit);
        }
        return out;
    }

    std::string NasRedisMetadataStore::key(std::string_view suffix) const
    {
        std::string prefix = config_.key_prefix.empty() ? "yuan:nas:" : config_.key_prefix;
        if (!prefix.empty() && prefix.back() != ':') {
            prefix.push_back(':');
        }
        return prefix + std::string(suffix);
    }

    std::string NasRedisMetadataStore::user_key(std::string_view user_id) const
    {
        return key("user:" + std::string(user_id));
    }

    std::string NasRedisMetadataStore::user_by_name_key(std::string_view username) const
    {
        return key("user_by_name:" + std::string(username));
    }

    std::string NasRedisMetadataStore::share_key(std::string_view share_id) const
    {
        return key("share:" + std::string(share_id));
    }

    std::string NasRedisMetadataStore::share_by_name_key(std::string_view share_name) const
    {
        return key("share_by_name:" + std::string(share_name));
    }

    std::string NasRedisMetadataStore::acl_key(std::string_view share_id) const
    {
        return key("acl:" + std::string(share_id));
    }

    std::string NasRedisMetadataStore::dead_property_key(std::string_view share_id, std::string_view path) const
    {
        return key("webdav:prop:" + std::string(share_id) + ":" + normalize_path_key(path));
    }

    std::string NasRedisMetadataStore::lock_key(std::string_view token) const
    {
        return key("webdav:lock:" + std::string(token));
    }

    std::string NasRedisMetadataStore::lock_guard_key(std::string_view token) const
    {
        return key("webdav:lock_guard:" + std::string(token));
    }

    std::string NasRedisMetadataStore::lock_index_key(std::string_view share_id) const
    {
        return key("webdav:locks:" + std::string(share_id));
    }

    std::string NasRedisMetadataStore::admin_session_key(std::string_view session_id) const
    {
        return key("admin:session:" + std::string(session_id));
    }

    std::optional<NasUser> NasRedisMetadataStore::load_user_by_id(std::string_view user_id) const
    {
        const auto fields = redis_hash(client_->hgetall(user_key(user_id)));
        if (fields.empty()) {
            return std::nullopt;
        }
        NasUser user;
        user.id = fields.contains("id") ? fields.at("id") : std::string(user_id);
        user.username = fields.contains("username") ? fields.at("username") : "";
        user.password_hash = fields.contains("password_hash") ? fields.at("password_hash") : "";
        user.smb_password_hash = fields.contains("smb_password_hash") ? fields.at("smb_password_hash") : "";
        user.enabled = !fields.contains("enabled") || fields.at("enabled") == "1";
        user.admin = fields.contains("admin") && fields.at("admin") == "1";
        return user;
    }

    std::optional<NasShare> NasRedisMetadataStore::load_share_by_id(std::string_view share_id) const
    {
        const auto fields = redis_hash(client_->hgetall(share_key(share_id)));
        if (fields.empty()) {
            return std::nullopt;
        }
        NasShare share;
        share.id = fields.contains("id") ? fields.at("id") : std::string(share_id);
        share.name = fields.contains("name") ? fields.at("name") : "";
        share.root_path = fields.contains("root_path") ? fields.at("root_path") : "";
        share.enabled = !fields.contains("enabled") || fields.at("enabled") == "1";
        share.readonly = fields.contains("readonly") && fields.at("readonly") == "1";
        share.default_permissions = fields.contains("default_permissions")
                                        ? permission_from_string(fields.at("default_permissions"))
                                        : NasPermission::read;
        const auto acl = redis_hash(client_->hgetall(acl_key(share.id)));
        for (const auto &[subject, mask] : acl) {
            share.subject_permissions[subject] = permission_from_string(mask);
        }
        return share;
    }

    std::optional<NasWebDavLockRecord> NasRedisMetadataStore::load_lock_by_token(std::string_view token) const
    {
        const auto fields = redis_hash(client_->hgetall(lock_key(token)));
        if (fields.empty()) {
            return std::nullopt;
        }
        NasWebDavLockRecord lock;
        lock.token = fields.contains("token") ? fields.at("token") : std::string(token);
        lock.share_id = fields.contains("share_id") ? fields.at("share_id") : "";
        lock.path = fields.contains("path") ? fields.at("path") : "";
        lock.scope = fields.contains("scope") ? fields.at("scope") : "exclusive";
        lock.depth = fields.contains("depth") ? fields.at("depth") : "infinity";
        lock.owner = fields.contains("owner") ? fields.at("owner") : "";
        lock.expires_at_unix_ms = fields.contains("expires_at_unix_ms")
                                      ? static_cast<std::int64_t>(std::strtoll(fields.at("expires_at_unix_ms").c_str(), nullptr, 10))
                                      : 0;
        return lock;
    }

    std::optional<NasAdminSession> NasRedisMetadataStore::load_admin_session_by_id(std::string_view session_id) const
    {
        const auto fields = redis_hash(client_->hgetall(admin_session_key(session_id)));
        if (fields.empty()) {
            return std::nullopt;
        }
        NasAdminSession session;
        session.id = fields.contains("id") ? fields.at("id") : std::string(session_id);
        session.username = fields.contains("username") ? fields.at("username") : "";
        session.remote_addr = fields.contains("remote_addr") ? fields.at("remote_addr") : "";
        session.user_agent = fields.contains("user_agent") ? fields.at("user_agent") : "";
        session.last_path = fields.contains("last_path") ? fields.at("last_path") : "";
        session.created_at_unix_ms = fields.contains("created_at_unix_ms")
                                         ? static_cast<std::int64_t>(std::strtoll(fields.at("created_at_unix_ms").c_str(), nullptr, 10))
                                         : 0;
        session.last_seen_unix_ms = fields.contains("last_seen_unix_ms")
                                        ? static_cast<std::int64_t>(std::strtoll(fields.at("last_seen_unix_ms").c_str(), nullptr, 10))
                                        : 0;
        session.request_count = fields.contains("request_count")
                                    ? static_cast<std::uint64_t>(std::strtoull(fields.at("request_count").c_str(), nullptr, 10))
                                    : 0;
        return session;
    }

    std::string NasRedisMetadataStore::normalize_path_key(std::string_view path)
    {
        std::string out(path);
        std::replace(out.begin(), out.end(), '\\', '/');
        while (!out.empty() && out.front() == '/') {
            out.erase(out.begin());
        }
        return out.empty() ? "." : out;
    }

    std::string NasRedisMetadataStore::permission_to_string(NasPermission permissions)
    {
        return std::to_string(static_cast<std::uint32_t>(permissions));
    }

    NasPermission NasRedisMetadataStore::permission_from_string(std::string_view text)
    {
        return static_cast<NasPermission>(static_cast<std::uint32_t>(std::strtoul(std::string(text).c_str(), nullptr, 10)));
    }
}
