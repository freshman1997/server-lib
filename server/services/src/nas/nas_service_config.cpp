#include "nas/nas_service_config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace yuan::server
{
    namespace
    {
        bool json_bool(const nlohmann::json &j, const char *key, bool fallback)
        {
            return j.contains(key) ? j.at(key).get<bool>() : fallback;
        }

        int json_int(const nlohmann::json &j, const char *key, int fallback)
        {
            return j.contains(key) ? j.at(key).get<int>() : fallback;
        }

        std::string json_string(const nlohmann::json &j, const char *key, std::string fallback = {})
        {
            return j.contains(key) ? j.at(key).get<std::string>() : std::move(fallback);
        }

        yuan::server::nas::NasPermission parse_permissions(const nlohmann::json &value,
                                                           yuan::server::nas::NasPermission fallback)
        {
            using yuan::server::nas::NasPermission;
            if (value.is_null()) {
                return fallback;
            }
            if (value.is_number_unsigned() || value.is_number_integer()) {
                return static_cast<NasPermission>(value.get<std::uint32_t>());
            }

            auto add_one = [](NasPermission &out, const std::string &text) {
                if (text == "read") out = out | NasPermission::read;
                else if (text == "write") out = out | NasPermission::write;
                else if (text == "remove" || text == "delete") out = out | NasPermission::remove;
                else if (text == "admin") out = out | NasPermission::admin;
            };

            NasPermission out = NasPermission::none;
            if (value.is_array()) {
                for (const auto &item : value) {
                    add_one(out, item.get<std::string>());
                }
                return out;
            }
            if (value.is_string()) {
                std::stringstream ss(value.get<std::string>());
                std::string item;
                while (std::getline(ss, item, ',')) {
                    const auto first = item.find_first_not_of(" \t");
                    if (first == std::string::npos) {
                        continue;
                    }
                    item.erase(0, first);
                    item.erase(item.find_last_not_of(" \t") + 1);
                    add_one(out, item);
                }
                return out;
            }
            return fallback;
        }

        std::string resolve_config_path(const std::filesystem::path &config_dir, const std::string &value)
        {
            if (value.empty()) {
                return {};
            }
            std::filesystem::path candidate(value);
            return candidate.is_relative() ? (config_dir / candidate).lexically_normal().string()
                                           : value;
        }

        void parse_http_config(const nlohmann::json &http, NasServiceConfig &config)
        {
            config.http.thread_pool_size = json_int(http, "thread_pool_size", config.http.thread_pool_size);
            config.http.enable_ssl = json_bool(http, "enable_ssl", config.http.enable_ssl);
            config.http.enable_cors = json_bool(http, "enable_cors", config.http.enable_cors);
            config.http.enable_keep_alive = json_bool(http, "enable_keep_alive", config.http.enable_keep_alive);
            config.http.enable_http2 = json_bool(http, "enable_http2", config.http.enable_http2);
            config.http.enable_http3 = json_bool(http, "enable_http3", config.http.enable_http3);
            if (http.contains("max_body_size")) {
                config.http.max_body_size = http.at("max_body_size").get<std::size_t>();
            }
            config.http.server_name = json_string(http, "server_name", config.http.server_name);
        }

        void parse_redis_config(const nlohmann::json &redis, NasServiceConfig &config)
        {
            config.nas.redis.enabled = json_bool(redis, "enabled", config.nas.redis.enabled);
            config.nas.redis.host = json_string(redis, "host", config.nas.redis.host);
            config.nas.redis.port = json_int(redis, "port", config.nas.redis.port);
            config.nas.redis.password = json_string(redis, "password", config.nas.redis.password);
            config.nas.redis.db = json_int(redis, "db", config.nas.redis.db);
            config.nas.redis.key_prefix = json_string(redis, "key_prefix", config.nas.redis.key_prefix);
            if (redis.contains("audit_max_events")) {
                config.nas.redis.audit_max_events = redis.at("audit_max_events").get<std::size_t>();
            }
        }

        void parse_audit_config(const nlohmann::json &audit, NasServiceConfig &config)
        {
            config.nas.audit.file_enabled = json_bool(audit, "file_enabled", config.nas.audit.file_enabled);
            config.nas.audit.file_path = json_string(audit, "file_path", config.nas.audit.file_path);
            if (audit.contains("max_events")) {
                config.nas.audit.max_events = audit.at("max_events").get<std::size_t>();
            }
            config.nas.redis.audit_max_events = config.nas.audit.max_events;
        }

        void parse_shares(const nlohmann::json &shares, NasServiceConfig &config)
        {
            for (const auto &item : shares) {
                yuan::server::nas::NasShare share;
                share.id = json_string(item, "id");
                share.name = json_string(item, "name");
                share.root_path = json_string(item, "root_path");
#ifdef _WIN32
                share.root_path = json_string(item, "windows_root_path", share.root_path);
#else
                share.root_path = json_string(item, "unix_root_path", share.root_path);
#endif
                const auto root_env_name = json_string(item, "root_env");
                if (!root_env_name.empty()) {
                    if (const char *root_env = std::getenv(root_env_name.c_str())) {
                        if (*root_env != '\0') {
                            share.root_path = root_env;
                        }
                    }
                }
                share.enabled = json_bool(item, "enabled", share.enabled);
                share.readonly = json_bool(item, "readonly", share.readonly);
                if (item.contains("default_permissions")) {
                    share.default_permissions = parse_permissions(item.at("default_permissions"), share.default_permissions);
                }
                if (item.contains("subject_permissions") && item.at("subject_permissions").is_object()) {
                    for (const auto &[subject, permissions] : item.at("subject_permissions").items()) {
                        if (!subject.empty()) {
                            share.subject_permissions[subject] = parse_permissions(permissions, yuan::server::nas::NasPermission::none);
                        }
                    }
                }
                if (!share.id.empty() && !share.name.empty() && !share.root_path.empty()) {
                    config.nas.shares.push_back(std::move(share));
                }
            }
        }

        void parse_users(const nlohmann::json &users, NasServiceConfig &config)
        {
            for (const auto &item : users) {
                yuan::server::nas::NasUser user;
                user.id = json_string(item, "id");
                user.username = json_string(item, "username");
                user.password_hash = json_string(item, "password_hash");
                user.smb_password_hash = json_string(item, "smb_password_hash");
                user.enabled = json_bool(item, "enabled", user.enabled);
                user.admin = json_bool(item, "admin", user.admin);
                if (!user.id.empty() && !user.username.empty() && !user.password_hash.empty()) {
                    config.bootstrap_users.push_back(std::move(user));
                }
            }
        }

        void parse_nas_config(const nlohmann::json &nas,
                              const std::filesystem::path &config_dir,
                              NasServiceConfig &config)
        {
            config.nas.webdav_mount = json_string(nas, "webdav_mount", config.nas.webdav_mount);
            config.nas.admin_console_path = json_string(nas, "admin_console_path", config.nas.admin_console_path);
            config.nas.allow_anonymous_read = json_bool(nas, "allow_anonymous_read", config.nas.allow_anonymous_read);

            if (nas.contains("redis") && nas.at("redis").is_object()) {
                parse_redis_config(nas.at("redis"), config);
            }
            if (nas.contains("audit") && nas.at("audit").is_object()) {
                parse_audit_config(nas.at("audit"), config);
            }
            if (nas.contains("shares") && nas.at("shares").is_array()) {
                parse_shares(nas.at("shares"), config);
            }
            if (nas.contains("users") && nas.at("users").is_array()) {
                parse_users(nas.at("users"), config);
            }

            if (config.nas.audit.file_enabled && !config.nas.audit.file_path.empty()) {
                config.nas.audit.file_path = resolve_config_path(config_dir, config.nas.audit.file_path);
            }
            if (!config.nas.admin_console_path.empty()) {
                config.nas.admin_console_path = resolve_config_path(config_dir, config.nas.admin_console_path);
            }
        }

        void parse_smb_config(const nlohmann::json &smb, NasServiceConfig &config)
        {
            config.smb.enabled = json_bool(smb, "enabled", config.smb.enabled);
            config.smb.port = json_int(smb, "port", config.smb.port);
            config.smb.require_signing = json_bool(smb, "require_signing", config.smb.require_signing);
            config.smb.enable_encryption = json_bool(smb, "enable_encryption", config.smb.enable_encryption);
            config.smb.server_name = json_string(smb, "server_name", config.smb.server_name);
            config.smb.domain_name = json_string(smb, "domain_name", config.smb.domain_name);
            if (config.smb.port <= 0) {
                config.smb.port = 445;
            }
        }
    }

    std::optional<NasServiceConfig> load_nas_service_config(const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return std::nullopt;
        }

        nlohmann::json j;
        try {
            in >> j;
        } catch (...) {
            return std::nullopt;
        }

        NasServiceConfig config;
        try {
            config.production_mode = json_bool(j, "production_mode", config.production_mode);
            config.port = json_int(j, "port", config.port);
            const auto config_dir = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();

            if (j.contains("http") && j.at("http").is_object()) {
                parse_http_config(j.at("http"), config);
            }
            if (j.contains("nas") && j.at("nas").is_object()) {
                parse_nas_config(j.at("nas"), config_dir, config);
            }
            if (j.contains("smb") && j.at("smb").is_object()) {
                parse_smb_config(j.at("smb"), config);
            }
        } catch (...) {
            return std::nullopt;
        }

        return config;
    }
}
