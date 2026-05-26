#include "nas/nas_service.h"

#include "nas/nas_smb_adapter.h"
#include "nas/nas_service_readiness.h"
#include "smb/smb_service.h"

#include "request.h"
#include "response.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <system_error>
#include <functional>
#include <unordered_map>

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

        bool require_admin(yuan::net::http::HttpRequest *req,
                           yuan::net::http::HttpResponse *resp,
                           const std::shared_ptr<yuan::server::nas::NasMetadataStore> &metadata,
                           std::string *actor = nullptr)
        {
            const auto *authorization = req ? req->get_header("authorization") : nullptr;
            yuan::server::nas::NasAuthService auth(metadata);
            auto result = authorization ? auth.authenticate_basic_header(*authorization) : yuan::server::nas::NasAuthResult{};
            if (!result.authenticated) {
                resp->add_header("WWW-Authenticate", "Basic realm=\"Yuan NAS Admin\"");
                resp->json("{\"error\":\"unauthorized\"}", yuan::net::http::ResponseCode::unauthorized);
                resp->send();
                return false;
            }
            if (!result.user.admin) {
                resp->json("{\"error\":\"forbidden\"}", yuan::net::http::ResponseCode::forbidden);
                resp->send();
                return false;
            }
            if (actor) {
                *actor = result.user.username;
            }
            return true;
        }

        std::int64_t unix_ms_now()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        yuan::server::nas::NasAuditEvent make_audit_event(std::string actor,
                                                          std::string action,
                                                          std::string target,
                                                          std::string detail = {})
        {
            yuan::server::nas::NasAuditEvent event;
            event.timestamp_unix_ms = unix_ms_now();
            event.actor = std::move(actor);
            event.action = std::move(action);
            event.target = std::move(target);
            event.detail = std::move(detail);
            return event;
        }

        nlohmann::json audit_json(const yuan::server::nas::NasAuditEvent &event)
        {
            nlohmann::json item;
            item["timestamp_unix_ms"] = event.timestamp_unix_ms;
            item["actor"] = event.actor;
            item["action"] = event.action;
            item["target"] = event.target;
            item["detail"] = event.detail;
            return item;
        }

        std::optional<yuan::server::nas::NasAuditEvent> audit_from_json(const std::string &line)
        {
            try {
                const auto item = nlohmann::json::parse(line);
                yuan::server::nas::NasAuditEvent event;
                event.timestamp_unix_ms = item.value("timestamp_unix_ms", 0ll);
                event.actor = item.value("actor", "");
                event.action = item.value("action", "");
                event.target = item.value("target", "");
                event.detail = item.value("detail", "");
                return event;
            } catch (...) {
                return std::nullopt;
            }
        }

        std::string request_header(const yuan::net::http::HttpRequest *req, const char *name)
        {
            if (!req) {
                return {};
            }
            const auto *value = req->get_header(name);
            return value ? *value : "";
        }

        std::string admin_remote_addr(const yuan::net::http::HttpRequest *req)
        {
            auto forwarded = request_header(req, "x-forwarded-for");
            if (!forwarded.empty()) {
                const auto comma = forwarded.find(',');
                if (comma != std::string::npos) {
                    forwarded.resize(comma);
                }
                return forwarded;
            }
            auto real_ip = request_header(req, "x-real-ip");
            return real_ip.empty() ? "local" : real_ip;
        }

        std::string admin_path(const yuan::net::http::HttpRequest *req)
        {
            return req ? req->get_raw_url() : "";
        }

        std::string admin_session_id(std::string_view username, std::string_view remote_addr)
        {
            const auto key = std::string(username) + "|" + std::string(remote_addr);
            return std::string(username) + ":" + std::to_string(std::hash<std::string>{}(key));
        }

        std::string request_body(yuan::net::http::HttpRequest *req)
        {
            if (!req) {
                return {};
            }
            if (req->body_buffer_size() > 0) {
                return req->body_buffer_text();
            }
            const char *begin = req->body_begin();
            const char *end = req->body_end();
            if (!begin || !end || end < begin) {
                return {};
            }
            return std::string(begin, end);
        }

        std::optional<nlohmann::json> request_json(yuan::net::http::HttpRequest *req)
        {
            try {
                return nlohmann::json::parse(request_body(req));
            } catch (...) {
                return std::nullopt;
            }
        }

        constexpr std::size_t kAdminMaxJsonBytes = 64 * 1024;
        constexpr int kAdminDefaultLimit = 100;
        constexpr int kAdminMaxLimit = 500;
        constexpr std::uint32_t kAdminRateLimitPerMinute = 240;

        std::optional<nlohmann::json> request_json_bounded(yuan::net::http::HttpRequest *req,
                                                           std::size_t max_bytes)
        {
            if (!req) {
                return std::nullopt;
            }
            const auto bytes = req->body_buffer_size() > 0 ? req->body_buffer_size() : req->get_body_length();
            if (bytes > max_bytes) {
                return std::nullopt;
            }
            return request_json(req);
        }

        int bounded_limit(yuan::net::http::HttpRequest *req,
                          int fallback = kAdminDefaultLimit,
                          int max_limit = kAdminMaxLimit)
        {
            if (!req) {
                return fallback;
            }
            int limit = req->get_param_int("limit", fallback);
            if (limit <= 0) {
                return fallback;
            }
            if (limit > max_limit) {
                return max_limit;
            }
            return limit;
        }

        bool enforce_admin_rate_limit(yuan::net::http::HttpRequest *req,
                                      yuan::net::http::HttpResponse *resp,
                                      std::string_view actor)
        {
            if (!req || !resp || actor.empty()) {
                return true;
            }

            struct AdminRateWindow
            {
                std::int64_t minute_window = 0;
                std::uint32_t request_count = 0;
            };

            static std::mutex rate_mutex;
            static std::unordered_map<std::string, AdminRateWindow> windows;

            const auto now_ms = unix_ms_now();
            const auto minute = now_ms / 60000;
            const std::string key = std::string(actor) + "|" + admin_remote_addr(req);

            std::lock_guard<std::mutex> lock(rate_mutex);
            auto &window = windows[key];
            if (window.minute_window != minute) {
                window.minute_window = minute;
                window.request_count = 0;
            }
            if (window.request_count >= kAdminRateLimitPerMinute) {
                nlohmann::json body;
                body["error"] = "too_many_requests";
                body["retry_after_seconds"] = 60;
                resp->json(body.dump(), yuan::net::http::ResponseCode::too_many_requests);
                resp->add_header("Retry-After", "60");
                resp->send();
                return false;
            }
            ++window.request_count;
            return true;
        }

        void json_error(yuan::net::http::HttpResponse *resp,
                        yuan::net::http::ResponseCode code,
                        std::string_view message)
        {
            nlohmann::json body;
            body["error"] = message;
            resp->json(body.dump(), code);
            resp->send();
        }

        bool ensure_admin_get_or_default(yuan::net::http::HttpRequest *req,
                                         yuan::net::http::HttpResponse *resp)
        {
            if (req && req->get_method() != yuan::net::http::HttpMethod::get_) {
                json_error(resp, yuan::net::http::ResponseCode::method_not_allowed, "method_not_allowed");
                return false;
            }
            return true;
        }

        bool ensure_admin_post(yuan::net::http::HttpRequest *req,
                               yuan::net::http::HttpResponse *resp)
        {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                json_error(resp, yuan::net::http::ResponseCode::method_not_allowed, "method_not_allowed");
                return false;
            }
            return true;
        }

        void upsert_config_user(std::vector<yuan::server::nas::NasUser> &users,
                                const yuan::server::nas::NasUser &user)
        {
            auto it = std::find_if(users.begin(), users.end(), [&](const auto &item) {
                return item.id == user.id || item.username == user.username;
            });
            if (it == users.end()) {
                users.push_back(user);
            } else {
                *it = user;
            }
        }

        void upsert_config_share(std::vector<yuan::server::nas::NasShare> &shares,
                                 const yuan::server::nas::NasShare &share)
        {
            auto it = std::find_if(shares.begin(), shares.end(), [&](const auto &item) {
                return item.id == share.id || item.name == share.name;
            });
            if (it == shares.end()) {
                shares.push_back(share);
            } else {
                *it = share;
            }
        }

        std::uint64_t directory_used_bytes(const std::filesystem::path &root)
        {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec)) {
                return 0;
            }
            if (std::filesystem::is_regular_file(root, ec)) {
                return std::filesystem::file_size(root, ec);
            }
            if (!std::filesystem::is_directory(root, ec)) {
                return 0;
            }

            std::uint64_t used = 0;
            std::filesystem::recursive_directory_iterator it(
                root,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec)) {
                    used += static_cast<std::uint64_t>(it->file_size(ec));
                }
            }
            return used;
        }

        nlohmann::json share_quota_json(const yuan::server::nas::NasShare &share)
        {
            std::error_code ec;
            const std::filesystem::path root(share.root_path);
            const auto exists = std::filesystem::exists(root, ec);
            const auto space = std::filesystem::space(root, ec);

            nlohmann::json item;
            item["id"] = share.id;
            item["name"] = share.name;
            item["root_path"] = share.root_path;
            item["enabled"] = share.enabled;
            item["exists"] = exists;
            item["used_bytes"] = directory_used_bytes(root);
            if (!ec) {
                item["capacity_bytes"] = static_cast<std::uint64_t>(space.capacity);
                item["free_bytes"] = static_cast<std::uint64_t>(space.free);
                item["available_bytes"] = static_cast<std::uint64_t>(space.available);
            } else {
                item["capacity_bytes"] = 0;
                item["free_bytes"] = 0;
                item["available_bytes"] = 0;
                item["error"] = ec.message();
            }
            return item;
        }

        void trim_audit_file_to_cap(const std::filesystem::path &audit_path, std::size_t cap)
        {
            if (cap == 0) {
                return;
            }

            std::ifstream in(audit_path, std::ios::binary);
            if (!in.good()) {
                return;
            }

            std::deque<std::string> recent;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) {
                    continue;
                }
                recent.push_back(std::move(line));
                while (recent.size() > cap) {
                    recent.pop_front();
                }
            }

            std::ofstream rewrite(audit_path, std::ios::binary | std::ios::trunc);
            if (!rewrite.good()) {
                return;
            }
            for (const auto &entry : recent) {
                rewrite << entry << '\n';
            }
        }

        constexpr std::string_view kNasAdminConsoleFallbackHtml =
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Yuan NAS Admin</title></head><body>"
            "<h1>Yuan NAS Admin</h1><p>Probe</p><p>Create User</p><p>Create Share</p>"
            "<p>Quota</p><p>Activity</p><p>Sessions</p><p>Audit</p>"
            "</body></html>";

        std::string load_nas_admin_console_html(const std::string &configured_path)
        {
            const auto source_dir = std::filesystem::path(__FILE__).parent_path().parent_path();
            const auto cwd = std::filesystem::current_path();
            std::vector<std::filesystem::path> candidates;
            if (!configured_path.empty()) {
                candidates.emplace_back(configured_path);
            }
            const std::array<std::filesystem::path, 4> defaults = {
                source_dir / "resources" / "nas_admin_console.html",
                cwd / "resources" / "nas_admin_console.html",
                cwd / "server" / "services" / "resources" / "nas_admin_console.html",
                cwd / ".." / "server" / "services" / "resources" / "nas_admin_console.html"
            };
            candidates.insert(candidates.end(), defaults.begin(), defaults.end());

            for (const auto &path : candidates) {
                std::ifstream in(path, std::ios::binary);
                if (!in.good()) {
                    continue;
                }
                std::ostringstream out;
                out << in.rdbuf();
                const auto html = out.str();
                if (!html.empty()) {
                    return html;
                }
            }

            return std::string(kNasAdminConsoleFallbackHtml);
        }
    }

    NasService::NasService(NasServiceConfig config)
        : config_(std::move(config))
    {
    }

    NasService::~NasService()
    {
        stop();
    }

    bool NasService::init()
    {
        if (!prepare_metadata()) {
            return false;
        }
        if (!apply_bootstrap_data()) {
            return false;
        }
        if (config_.production_mode && !production_startup_check()) {
            return false;
        }

        http_ = std::make_unique<HttpService>(config_.port, config_.http);
        if (has_runtime_context_) {
            http_->set_runtime_context(runtime_context_);
        }
        if (!http_->init()) {
            http_.reset();
            return false;
        }

        if (!init_smb_service()) {
            http_->stop();
            http_.reset();
            return false;
        }

        mount_result_ = yuan::server::nas::mount_nas_webdav(http_->server(), config_.nas, config_.metadata);
        install_health_endpoint();
        install_admin_endpoints();
        install_admin_console();
        mounted_ = true;
        initialized_ = true;
        return true;
    }

    std::vector<yuan::server::nas::NasShare> NasService::effective_shares() const
    {
        if (config_.metadata && config_.metadata->available()) {
            auto stored_shares = config_.metadata->list_shares();
            if (!stored_shares.empty()) {
                return stored_shares;
            }
        }
        return config_.nas.shares;
    }

    bool NasService::prepare_metadata()
    {
        if (!config_.metadata) {
#if YUAN_NAS_HAS_REDIS
            if (config_.nas.redis.enabled) {
                auto redis_cfg = config_.nas.redis;
                redis_cfg.audit_max_events = config_.nas.audit.max_events;
                auto redis = std::make_shared<yuan::server::nas::NasRedisMetadataStore>(std::move(redis_cfg));
                if (!redis->init()) {
                    return false;
                }
                config_.metadata = std::move(redis);
            }
#endif
        }

        return config_.metadata && config_.metadata->available();
    }

    bool NasService::apply_bootstrap_data()
    {
        if (!config_.metadata || !config_.metadata->available()) {
            return false;
        }
        for (const auto &user : config_.bootstrap_users) {
            if (!config_.metadata->upsert_user(user)) {
                return false;
            }
        }
        for (const auto &share : config_.nas.shares) {
            (void)config_.metadata->upsert_share(share);
        }
        return true;
    }

    bool NasService::production_startup_check() const
    {
        const auto metadata_available = config_.metadata && config_.metadata->available();
        const auto body = build_nas_service_readiness_json({
            config_,
            true,
            true,
            true,
            metadata_available,
            metadata_available ? config_.metadata->list_users() : std::vector<yuan::server::nas::NasUser>{},
            effective_shares()
        });
        return body.value("ready", false);
    }

    nlohmann::json NasService::health_status_json() const
    {
        const auto shares = effective_shares();
        nlohmann::json body;
        body["ok"] = initialized_ && config_.metadata && config_.metadata->available();
        body["initialized"] = initialized_;
        body["started"] = started_;
        body["mounted"] = mounted_;
        body["metadata_available"] = config_.metadata && config_.metadata->available();
        body["webdav_mount"] = config_.nas.webdav_mount;
        body["share_count"] = config_.nas.shares.size();
        body["effective_share_count"] = shares.size();
        body["configured_share_count"] = config_.nas.shares.size();
        body["redis_enabled"] = config_.nas.redis.enabled;
        body["audit_file_enabled"] = config_.nas.audit.file_enabled;
        body["smb_enabled"] = config_.smb.enabled;
        body["smb_started"] = smb_ != nullptr;
        body["smb_port"] = config_.smb.port;
        body["smb_require_signing"] = config_.smb.require_signing;
        return body;
    }

    yuan::net::smb::SmbServerConfig NasService::build_smb_server_config() const
    {
        yuan::net::smb::SmbServerConfig base;
        base.port = static_cast<uint16_t>(std::max(1, config_.smb.port));
        base.server_name = config_.smb.server_name;
        base.domain_name = config_.smb.domain_name;
        base.require_signing = config_.smb.require_signing;
        base.enable_encryption = config_.smb.enable_encryption;
        return make_smb_config_from_nas(config_.nas, config_.metadata, std::move(base));
    }

    bool NasService::init_smb_service()
    {
        stop_smb_service();
        if (!config_.smb.enabled) {
            return true;
        }

        auto smb_cfg = build_smb_server_config();
        smb_ = std::make_unique<SmbService>(config_.smb.port, smb_cfg);
        smb_handler_ = std::make_unique<NasSmbHandler>(config_.metadata);
        smb_->set_handler(smb_handler_.get());
        if (has_runtime_context_) {
            smb_->set_runtime_context(runtime_context_);
        }
        if (!smb_->init()) {
            smb_.reset();
            smb_handler_.reset();
            return false;
        }
        return true;
    }

    void NasService::stop_smb_service()
    {
        if (smb_) {
            smb_->stop();
            smb_.reset();
        }
        smb_handler_.reset();
    }

    void NasService::refresh_smb_shares()
    {
        if (!smb_) {
            return;
        }

        auto cfg = build_smb_server_config();
        auto &mgr = smb_->server().share_manager();
        for (auto *share : mgr.list_shares()) {
            if (share && share->type() == yuan::net::smb::ShareType::DISK && share->name() != "IPC$") {
                mgr.remove_share(share->name());
            }
        }
        for (const auto &share_cfg : cfg.shares) {
            if (share_cfg.name != "IPC$") {
                mgr.add_share(share_cfg);
            }
        }
    }

    void NasService::record_audit(std::string actor,
                                  std::string action,
                                  std::string target,
                                  std::string detail)
    {
        auto event = make_audit_event(std::move(actor), std::move(action), std::move(target), std::move(detail));
        if (config_.metadata && config_.metadata->append_audit_event(event)) {
            return;
        }
        if (!config_.nas.audit.file_enabled || config_.nas.audit.file_path.empty()) {
            return;
        }

        std::error_code ec;
        const std::filesystem::path audit_path(config_.nas.audit.file_path);
        if (audit_path.has_parent_path()) {
            std::filesystem::create_directories(audit_path.parent_path(), ec);
        }
        std::ofstream out(audit_path, std::ios::binary | std::ios::app);
        if (out.good()) {
            out << audit_json(event).dump() << '\n';
        }
        trim_audit_file_to_cap(audit_path, config_.nas.audit.max_events);
    }

    std::vector<yuan::server::nas::NasAuditEvent> NasService::audit_events(std::size_t limit) const
    {
        if (limit == 0) {
            return {};
        }
        const auto effective_limit = config_.nas.audit.max_events > 0
            ? (std::min)(limit, config_.nas.audit.max_events)
            : limit;
        std::vector<yuan::server::nas::NasAuditEvent> out;
        if (config_.metadata) {
            out = config_.metadata->list_audit_events(effective_limit);
        }
        if (!config_.nas.audit.file_enabled || config_.nas.audit.file_path.empty()) {
            return out;
        }

        std::ifstream in(config_.nas.audit.file_path, std::ios::binary);
        if (!in.good()) {
            return out;
        }

        std::deque<yuan::server::nas::NasAuditEvent> recent;
        std::string line;
        while (std::getline(in, line)) {
            if (auto event = audit_from_json(line)) {
                recent.push_back(std::move(*event));
                while (recent.size() > effective_limit) {
                    recent.pop_front();
                }
            }
        }
        out.reserve(out.size() + recent.size());
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
            out.push_back(*it);
        }
        std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.timestamp_unix_ms > rhs.timestamp_unix_ms;
        });
        if (out.size() > effective_limit) {
            out.resize(effective_limit);
        }
        return out;
    }

    void NasService::record_admin_session(const yuan::net::http::HttpRequest *req, const std::string &username)
    {
        if (!config_.metadata || username.empty()) {
            return;
        }

        const auto remote_addr = admin_remote_addr(req);
        const auto id = admin_session_id(username, remote_addr);
        auto sessions = config_.metadata->list_admin_sessions(1000);
        yuan::server::nas::NasAdminSession session;
        auto it = std::find_if(sessions.begin(), sessions.end(), [&](const auto &item) {
            return item.id == id;
        });
        if (it != sessions.end()) {
            session = *it;
        } else {
            session.id = id;
            session.username = username;
            session.remote_addr = remote_addr;
            session.created_at_unix_ms = unix_ms_now();
        }
        session.username = username;
        session.remote_addr = remote_addr;
        session.user_agent = request_header(req, "user-agent");
        session.last_path = admin_path(req);
        session.last_seen_unix_ms = unix_ms_now();
        ++session.request_count;
        (void)config_.metadata->upsert_admin_session(session);
    }

    std::optional<std::string> NasService::guard_admin_request(yuan::net::http::HttpRequest *req,
                                                               yuan::net::http::HttpResponse *resp)
    {
        std::string actor;
        if (!require_admin(req, resp, config_.metadata, &actor)) {
            return std::nullopt;
        }
        if (!enforce_admin_rate_limit(req, resp, actor)) {
            return std::nullopt;
        }
        record_admin_session(req, actor);
        return actor;
    }

    void NasService::install_health_endpoint()
    {
        if (!http_) {
            return;
        }

        http_->server().on("/nas/health", [this](yuan::net::http::HttpRequest *, yuan::net::http::HttpResponse *resp) {
            auto body = health_status_json();
            resp->json(body.dump(), body["ok"].get<bool>() ? yuan::net::http::ResponseCode::ok_
                                                           : yuan::net::http::ResponseCode::service_unavailable);
            resp->send();
        });
    }

    void NasService::install_admin_endpoints()
    {
        if (!http_) {
            return;
        }

        http_->server().on("/nas/admin/shares", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_shares(req, resp);
        });

        http_->server().on("/nas/admin/users", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_users(req, resp);
        });

        http_->server().on("/nas/admin/quota", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_quota(req, resp);
        });

        http_->server().on("/nas/admin/health/actions", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_health_actions(req, resp);
        });

        http_->server().on("/nas/admin/audit", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_audit(req, resp);
        });

        http_->server().on("/nas/admin/sessions", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_sessions(req, resp);
        });

        http_->server().on("/nas/admin/activity", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_activity(req, resp);
        });

        http_->server().on("/nas/admin/readiness", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            handle_admin_readiness(req, resp);
        });
    }

    void NasService::handle_admin_shares(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp)
    {
        auto actor = guard_admin_request(req, resp);
        if (!actor) {
            return;
        }
        if (req && req->get_method() == yuan::net::http::HttpMethod::post_) {
            handle_admin_shares_post(req, resp, *actor);
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }
        handle_admin_shares_get(resp);
    }

    void NasService::handle_admin_shares_get(yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_shares_json();
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_shares_post(yuan::net::http::HttpRequest *req,
                                              yuan::net::http::HttpResponse *resp,
                                              const std::string &actor)
    {
        if (!ensure_admin_post(req, resp)) {
            return;
        }

        auto body = request_json_bounded(req, kAdminMaxJsonBytes);
        if (!body) {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "invalid_json");
            return;
        }
        yuan::server::nas::NasShare share;
        share.id = json_string(*body, "id");
        share.name = json_string(*body, "name");
        share.root_path = json_string(*body, "root_path");
        share.enabled = json_bool(*body, "enabled", share.enabled);
        share.readonly = json_bool(*body, "readonly", share.readonly);
        if (body->contains("default_permissions")) {
            share.default_permissions = parse_permissions(body->at("default_permissions"), share.default_permissions);
        }
        if (share.id.empty() || share.name.empty() || share.root_path.empty()) {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "missing_required_share_fields");
            return;
        }
        if (!config_.metadata || !config_.metadata->upsert_share(share)) {
            json_error(resp, yuan::net::http::ResponseCode::internal_server_error, "store_failed");
            return;
        }
        upsert_config_share(config_.nas.shares, share);
        if (mount_result_.share_manager) {
            mount_result_.share_manager->replace(effective_shares());
            mount_result_.share_count = mount_result_.share_manager->shares().size();
        }
        refresh_smb_shares();
        record_audit(actor, "share.upsert", share.name, share.root_path);
        nlohmann::json out;
        out["ok"] = true;
        out["id"] = share.id;
        out["name"] = share.name;
        resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_users(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp)
    {
        auto actor = guard_admin_request(req, resp);
        if (!actor) {
            return;
        }
        if (req && req->get_method() == yuan::net::http::HttpMethod::post_) {
            handle_admin_users_post(req, resp, *actor);
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }
        handle_admin_users_get(resp);
    }

    void NasService::handle_admin_users_get(yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_users_json();
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_users_post(yuan::net::http::HttpRequest *req,
                                             yuan::net::http::HttpResponse *resp,
                                             const std::string &actor)
    {
        if (!ensure_admin_post(req, resp)) {
            return;
        }

        auto body = request_json_bounded(req, kAdminMaxJsonBytes);
        if (!body) {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "invalid_json");
            return;
        }
        yuan::server::nas::NasUser user;
        user.id = json_string(*body, "id");
        user.username = json_string(*body, "username");
        user.password_hash = json_string(*body, "password_hash");
        if (user.password_hash.empty() && body->contains("password")) {
            user.password_hash = yuan::server::nas::NasAuthService::hash_password_for_config(
                body->at("password").get<std::string>(),
                json_string(*body, "salt", "nas"));
        }
        user.enabled = json_bool(*body, "enabled", user.enabled);
        user.admin = json_bool(*body, "admin", user.admin);
        if (user.id.empty() || user.username.empty() || user.password_hash.empty()) {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "missing_required_user_fields");
            return;
        }
        if (!config_.metadata || !config_.metadata->upsert_user(user)) {
            json_error(resp, yuan::net::http::ResponseCode::internal_server_error, "store_failed");
            return;
        }
        upsert_config_user(config_.bootstrap_users, user);
        record_audit(actor, "user.upsert", user.username, user.admin ? "admin" : "user");
        nlohmann::json out;
        out["ok"] = true;
        out["id"] = user.id;
        out["username"] = user.username;
        resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_quota(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp)
    {
        if (!guard_admin_request(req, resp)) {
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }

        auto body = build_admin_quota_json();
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_health_actions(yuan::net::http::HttpRequest *req,
                                                 yuan::net::http::HttpResponse *resp)
    {
        auto actor = guard_admin_request(req, resp);
        if (!actor) {
            return;
        }
        if (req && req->get_method() == yuan::net::http::HttpMethod::get_) {
            handle_admin_health_actions_get(resp);
            return;
        }
        if (!ensure_admin_post(req, resp)) {
            return;
        }
        handle_admin_health_actions_post(req, resp, *actor);
    }

    void NasService::handle_admin_health_actions_get(yuan::net::http::HttpResponse *resp) const
    {
        nlohmann::json body;
        body["actions"] = nlohmann::json::array({ "probe" });
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_health_actions_post(yuan::net::http::HttpRequest *req,
                                                      yuan::net::http::HttpResponse *resp,
                                                      const std::string &actor)
    {
        auto body = request_json_bounded(req, kAdminMaxJsonBytes);
        if (!body) {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "invalid_json");
            return;
        }
        const auto action = json_string(*body, "action");
        if (action != "probe") {
            json_error(resp, yuan::net::http::ResponseCode::bad_request, "unsupported_action");
            return;
        }

        record_audit(actor, "service.health_probe", config_.nas.webdav_mount, "admin action");
        nlohmann::json out;
        out["ok"] = true;
        out["action"] = action;
        out["health"] = health_status_json();
        resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_audit(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp)
    {
        if (!guard_admin_request(req, resp)) {
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }

        handle_admin_audit_get(req, resp);
    }

    void NasService::handle_admin_audit_get(yuan::net::http::HttpRequest *req,
                                            yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_audit_json(req);
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_sessions(yuan::net::http::HttpRequest *req,
                                           yuan::net::http::HttpResponse *resp)
    {
        if (!guard_admin_request(req, resp)) {
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }

        handle_admin_sessions_get(req, resp);
    }

    void NasService::handle_admin_sessions_get(yuan::net::http::HttpRequest *req,
                                               yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_sessions_json(req);
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_activity(yuan::net::http::HttpRequest *req,
                                           yuan::net::http::HttpResponse *resp)
    {
        if (!guard_admin_request(req, resp)) {
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }

        handle_admin_activity_get(resp);
    }

    void NasService::handle_admin_activity_get(yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_activity_json();
        resp->json(body.dump(), yuan::net::http::ResponseCode::ok_);
        resp->send();
    }

    void NasService::handle_admin_readiness(yuan::net::http::HttpRequest *req,
                                            yuan::net::http::HttpResponse *resp)
    {
        if (!guard_admin_request(req, resp)) {
            return;
        }
        if (!ensure_admin_get_or_default(req, resp)) {
            return;
        }

        handle_admin_readiness_get(resp);
    }

    void NasService::handle_admin_readiness_get(yuan::net::http::HttpResponse *resp) const
    {
        auto body = build_admin_readiness_json();
        const auto ready = body.value("ready", false);
        resp->json(body.dump(), ready ? yuan::net::http::ResponseCode::ok_
                                      : yuan::net::http::ResponseCode::service_unavailable);
        resp->send();
    }

    nlohmann::json NasService::build_admin_shares_json() const
    {
        nlohmann::json shares = nlohmann::json::array();
        for (const auto &share : config_.nas.shares) {
            nlohmann::json item;
            item["id"] = share.id;
            item["name"] = share.name;
            item["root_path"] = share.root_path;
            item["enabled"] = share.enabled;
            item["readonly"] = share.readonly;
            item["default_permissions"] = static_cast<std::uint32_t>(share.default_permissions);
            shares.push_back(std::move(item));
        }
        nlohmann::json body;
        body["shares"] = std::move(shares);
        body["count"] = config_.nas.shares.size();
        return body;
    }

    nlohmann::json NasService::build_admin_users_json() const
    {
        nlohmann::json users = nlohmann::json::array();
        const auto listed = config_.metadata ? config_.metadata->list_users() : std::vector<yuan::server::nas::NasUser>{};
        for (const auto &user : listed) {
            nlohmann::json item;
            item["id"] = user.id;
            item["username"] = user.username;
            item["enabled"] = user.enabled;
            item["admin"] = user.admin;
            users.push_back(std::move(item));
        }
        nlohmann::json body;
        body["users"] = std::move(users);
        body["count"] = listed.size();
        return body;
    }

    nlohmann::json NasService::build_admin_quota_json() const
    {
        nlohmann::json shares = nlohmann::json::array();
        std::uint64_t total_used = 0;
        for (const auto &share : effective_shares()) {
            auto item = share_quota_json(share);
            total_used += item.value("used_bytes", 0ull);
            shares.push_back(std::move(item));
        }

        nlohmann::json body;
        body["shares"] = std::move(shares);
        body["count"] = body["shares"].size();
        body["total_used_bytes"] = total_used;
        return body;
    }

    nlohmann::json NasService::build_admin_audit_json(yuan::net::http::HttpRequest *req) const
    {
        const auto events = audit_events(static_cast<std::size_t>(bounded_limit(req)));
        nlohmann::json items = nlohmann::json::array();
        for (const auto &event : events) {
            items.push_back(audit_json(event));
        }

        nlohmann::json body;
        body["events"] = std::move(items);
        body["count"] = body["events"].size();
        return body;
    }

    nlohmann::json NasService::build_admin_sessions_json(yuan::net::http::HttpRequest *req) const
    {
        const auto limit = static_cast<std::size_t>(bounded_limit(req));
        const auto sessions = config_.metadata
                                  ? config_.metadata->list_admin_sessions(limit)
                                  : std::vector<yuan::server::nas::NasAdminSession>{};
        nlohmann::json items = nlohmann::json::array();
        for (const auto &session : sessions) {
            nlohmann::json item;
            item["id"] = session.id;
            item["username"] = session.username;
            item["remote_addr"] = session.remote_addr;
            item["user_agent"] = session.user_agent;
            item["last_path"] = session.last_path;
            item["created_at_unix_ms"] = session.created_at_unix_ms;
            item["last_seen_unix_ms"] = session.last_seen_unix_ms;
            item["request_count"] = session.request_count;
            items.push_back(std::move(item));
        }

        nlohmann::json body;
        body["sessions"] = std::move(items);
        body["count"] = body["sessions"].size();
        return body;
    }

    nlohmann::json NasService::build_admin_activity_json() const
    {
        const auto users = config_.metadata ? config_.metadata->list_users() : std::vector<yuan::server::nas::NasUser>{};
        const auto shares = effective_shares();
        nlohmann::json body;
        body["initialized"] = initialized_;
        body["started"] = started_;
        body["mounted"] = mounted_;
        body["webdav_mount"] = config_.nas.webdav_mount;
        body["redis_enabled"] = config_.nas.redis.enabled;
        body["user_count"] = users.size();
        body["share_count"] = shares.size();
        body["metadata_available"] = config_.metadata && config_.metadata->available();
        return body;
    }

    nlohmann::json NasService::build_admin_readiness_json() const
    {
        const auto metadata_available = config_.metadata && config_.metadata->available();
        return build_nas_service_readiness_json({
            config_,
            initialized_,
            started_,
            mounted_,
            metadata_available,
            metadata_available ? config_.metadata->list_users() : std::vector<yuan::server::nas::NasUser>{},
            effective_shares()
        });
    }

    void NasService::install_admin_console()
    {
        if (!http_) {
            return;
        }

        const auto html = std::make_shared<std::string>(
            load_nas_admin_console_html(config_.nas.admin_console_path));

        http_->server().on("/nas/admin", [html](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (req && req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            resp->set_response_code(yuan::net::http::ResponseCode::ok_);
            resp->add_header("Content-Type", "text/html; charset=utf-8");
            resp->append_body(*html);
            resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
            resp->send();
        });
    }

    void NasService::start()
    {
        if (http_) {
            http_->start();
            started_ = true;
        }
        if (smb_) {
            smb_->start();
        }
    }

    void NasService::stop()
    {
        stop_smb_service();
        if (http_) {
            http_->stop();
            http_.reset();
        }
        started_ = false;
        mounted_ = false;
        initialized_ = false;
        mount_result_ = {};
    }

    void NasService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        runtime_context_ = context;
        has_runtime_context_ = true;
        if (http_) {
            http_->set_runtime_context(context);
        }
        if (smb_) {
            smb_->set_runtime_context(context);
        }
    }

    bool NasService::reload(NasServiceConfig config)
    {
        if (!config.metadata) {
            config.metadata = config_.metadata;
        }

        if (initialized_ &&
            (config.port != config_.port || config.nas.webdav_mount != config_.nas.webdav_mount)) {
            return false;
        }

        auto previous = std::move(config_);
        config_ = std::move(config);
        if (!prepare_metadata() || !apply_bootstrap_data()) {
            config_ = std::move(previous);
            return false;
        }

        if (mount_result_.share_manager) {
            mount_result_.share_manager->replace(effective_shares());
            mount_result_.share_count = mount_result_.share_manager->shares().size();
        }
        refresh_smb_shares();
        if (!initialized_) {
            return init();
        }

        if (config_.smb.enabled != previous.smb.enabled ||
            config_.smb.port != previous.smb.port ||
            config_.smb.require_signing != previous.smb.require_signing ||
            config_.smb.enable_encryption != previous.smb.enable_encryption ||
            config_.smb.server_name != previous.smb.server_name ||
            config_.smb.domain_name != previous.smb.domain_name) {
            const bool was_started = started_;
            stop_smb_service();
            if (!init_smb_service()) {
                config_ = std::move(previous);
                return false;
            }
            if (was_started && smb_) {
                smb_->start();
            }
        }

        record_audit("system", "service.reload", config_.nas.webdav_mount,
                     std::to_string(config_.nas.shares.size()) + " configured shares");
        return true;
    }

    bool NasService::reload_from_file(const std::filesystem::path &path)
    {
        auto loaded = load_nas_service_config(path);
        return loaded ? reload(std::move(*loaded)) : false;
    }

    HttpService &NasService::http_service()
    {
        return *http_;
    }

    const HttpService &NasService::http_service() const
    {
        return *http_;
    }

    std::shared_ptr<yuan::server::nas::NasMetadataStore> NasService::metadata_store() const
    {
        return config_.metadata;
    }

    const NasServiceConfig &NasService::config() const
    {
        return config_;
    }
}
