#include "nas/nas.h"

#include "base/utils/base64.h"
#include "context.h"
#include "request.h"
#include "response.h"
#include "http_server.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace
{
    class MemoryMetadataStore final : public yuan::server::nas::NasMetadataStore
    {
    public:
        bool available() const override { return true; }

        std::optional<yuan::server::nas::NasUser> find_user_by_name(std::string_view username) const override
        {
            auto it = users_by_name.find(std::string(username));
            return it == users_by_name.end() ? std::nullopt : std::optional<yuan::server::nas::NasUser>(it->second);
        }
        std::vector<yuan::server::nas::NasUser> list_users() const override
        {
            std::vector<yuan::server::nas::NasUser> out;
            for (const auto &[_, user] : users_by_name) {
                out.push_back(user);
            }
            return out;
        }

        bool upsert_user(const yuan::server::nas::NasUser &user) override
        {
            users_by_name[user.username] = user;
            return true;
        }

        std::optional<yuan::server::nas::NasShare> find_share_by_name(std::string_view) const override { return std::nullopt; }
        std::vector<yuan::server::nas::NasShare> list_shares() const override { return {}; }
        bool upsert_share(const yuan::server::nas::NasShare &) override { return true; }
        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override { return yuan::server::nas::NasPermission::none; }
        bool set_permissions(std::string_view, std::string_view, yuan::server::nas::NasPermission) override { return true; }
        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override { return {}; }
        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override { return true; }
        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override { return true; }
        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &) override { return true; }
        bool try_create_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &) override { return true; }
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view) const override { return std::nullopt; }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view, std::string_view) const override { return {}; }
        bool remove_webdav_lock(std::string_view) override { return true; }
        std::size_t prune_expired_webdav_locks() override { return 0; }
        bool append_audit_event(const yuan::server::nas::NasAuditEvent &event) override
        {
            audit.push_back(event);
            return true;
        }
        std::vector<yuan::server::nas::NasAuditEvent> list_audit_events(std::size_t limit) const override
        {
            std::vector<yuan::server::nas::NasAuditEvent> out;
            const auto start = audit.size() > limit ? audit.size() - limit : 0;
            for (auto i = audit.size(); i > start; --i) {
                out.push_back(audit[i - 1]);
            }
            return out;
        }
        bool upsert_admin_session(const yuan::server::nas::NasAdminSession &session) override
        {
            sessions[session.id] = session;
            return true;
        }
        std::vector<yuan::server::nas::NasAdminSession> list_admin_sessions(std::size_t limit) const override
        {
            std::vector<yuan::server::nas::NasAdminSession> out;
            for (const auto &[_, session] : sessions) {
                out.push_back(session);
            }
            std::sort(out.begin(), out.end(), [](const auto &lhs, const auto &rhs) {
                return lhs.last_seen_unix_ms > rhs.last_seen_unix_ms;
            });
            if (out.size() > limit) {
                out.resize(limit);
            }
            return out;
        }

        std::unordered_map<std::string, yuan::server::nas::NasUser> users_by_name;
        std::vector<yuan::server::nas::NasAuditEvent> audit;
        std::unordered_map<std::string, yuan::server::nas::NasAdminSession> sessions;
    };
}

namespace
{
    int failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++failed;
            std::cerr << "[FAIL] " << msg << '\n';
        }
    }
}

int main()
{
    namespace nas = yuan::server::nas;

    const auto root = std::filesystem::temp_directory_path() / "yuan_nas_core_share";
    nas::NasShare share;
    share.id = "share-1";
    share.name = "public";
    share.root_path = root.string();
    share.default_permissions = nas::NasPermission::read | nas::NasPermission::write;
    share.subject_permissions["alice"] = nas::NasPermission::read | nas::NasPermission::write | nas::NasPermission::remove;

    auto memory = std::make_shared<MemoryMetadataStore>();

    nas::NasShareManager shares({ share });
    auto found = shares.find_by_name("public");
    check(found.has_value(), "share lookup should find enabled share");

    auto resolved = shares.resolve("public", "/docs/readme.txt");
    check(resolved.has_value(), "share path should resolve");
    check(resolved->relative_path == "docs/readme.txt", "relative path should normalize");
    check(resolved->absolute_path.find(root.string()) == 0, "absolute path should stay under root");

    auto blocked = shares.resolve("public", "../../secret.txt");
    check(!blocked.has_value(), "path traversal should be rejected");

    nas::NasWebDavAdapter dav("/dav");
    auto route = dav.parse_route("/dav/public/docs/readme.txt");
    check(route && route->share_name == "public" && route->relative_path == "docs/readme.txt",
          "WebDAV route should extract share and relative path");
    check(!dav.parse_route("/dav/public/../../secret.txt"), "WebDAV route should reject traversal");
    auto dav_resolved = dav.resolve(shares, "/dav/public/docs/readme.txt");
    check(dav_resolved && dav_resolved->share.name == "public", "WebDAV route should resolve through share manager");

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    auto nas_backend = std::make_shared<nas::NasWebDavBackend>(std::make_shared<nas::NasShareManager>(shares), nullptr, "/dav");
    auto write_result = nas_backend->write("/public/docs/note.txt", "hello", true);
    check(write_result.ok, "NAS WebDAV backend should write into resolved share");
    auto read_result = nas_backend->read("/public/docs/note.txt");
    check(read_result && *read_result == "hello", "NAS WebDAV backend should read from resolved share");
    const auto source_file = root / "source-stream.bin";
    {
        std::ofstream out(source_file, std::ios::binary | std::ios::trunc);
        out << std::string(128 * 1024, 's');
    }
    auto stream_write_result = nas_backend->write_from_file("/public/docs/stream.bin", source_file, true);
    check(stream_write_result.ok, "NAS WebDAV backend should stream write from file");
    check(std::filesystem::file_size(root / "docs" / "stream.bin") == 128 * 1024,
          "NAS WebDAV backend stream write should preserve size");
    check(!nas_backend->write("/missing/docs/note.txt", "x", true).ok, "NAS WebDAV backend should reject unknown share");
    std::filesystem::remove_all(root, ec);

    yuan::net::http::HttpServer server;
    nas::NasConfig cfg;
    cfg.webdav_mount = "/dav";
    cfg.allow_anonymous_read = true;
    cfg.shares.push_back(share);
    auto mount_result = nas::mount_nas_webdav(server, cfg, memory);
    check(mount_result.mounted && mount_result.mount_path == "/dav" && mount_result.share_count == 1,
          "NAS WebDAV mount helper should register mount");

    nas::NasPermissionService permissions;
    check(permissions.allowed(share, "alice", nas::NasPermission::remove), "explicit ACL should allow alice delete");
    check(!permissions.allowed(share, "bob", nas::NasPermission::remove), "default ACL should deny bob delete");
    nas::NasUser admin_user;
    admin_user.id = "root";
    admin_user.admin = true;
    check(permissions.allowed(share, admin_user, nas::NasPermission::admin), "admin user should bypass share ACL");

    share.readonly = true;
    check(!permissions.allowed(share, "alice", nas::NasPermission::write), "readonly share should deny write");

    nas::NasUser user;
    user.id = "u1";
    user.username = "alice";
    user.password_hash = nas::NasAuthService::hash_password_for_config("secret", "salt");
    memory->upsert_user(user);

    nas::NasAuthService auth(memory);
    check(auth.authenticate_password("alice", "secret").authenticated, "password auth should accept valid password");
    check(!auth.authenticate_password("alice", "bad").authenticated, "password auth should reject invalid password");
    check(user.password_hash.rfind("pbkdf2-sha256$", 0) == 0,
          "config hash helper should generate pbkdf2-sha256 format");

    const std::string legacy_fallback = "fnv1a64$salt$" + [&] {
        std::uint64_t hash = 1469598103934665603ull;
        const std::string material = "salt:secret";
        for (unsigned char ch : material) {
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        std::ostringstream oss;
        oss << std::hex << hash;
        return oss.str();
    }();
    check(nas::NasAuthService::verify_password(legacy_fallback, "secret"),
          "legacy fnv1a64 format should still verify for migration");

    const std::string basic = "Basic " + yuan::base::util::base64_encode("alice:secret");
    check(auth.authenticate_basic_header(basic).authenticated, "basic auth should parse and authenticate");

    yuan::net::http::HttpSessionContext http_ctx(nullptr);
    auto *req = http_ctx.get_request();
    auto *resp = http_ctx.get_response();
    auto auth_mw = nas::nas_basic_auth_middleware(std::make_shared<nas::NasAuthService>(memory));
    req->set_method(yuan::net::http::HttpMethod::put_);
    check(nas::NasPermissionService::required_for_webdav_request(*req) == nas::NasPermission::write,
          "PUT should map to write permission");
    check(auth_mw->process(req, resp) == yuan::net::http::MiddlewareResult::unauthorized,
          "NAS auth middleware should reject missing credentials on write");
    req->add_header("Authorization", basic);
    check(auth_mw->process(req, resp) == yuan::net::http::MiddlewareResult::next,
          "NAS auth middleware should accept valid Basic credentials");
    check(req->get_header("x-nas-user-id") && *req->get_header("x-nas-user-id") == "u1",
          "NAS auth middleware should attach authenticated user context");

    nas::NasHttpAuthOptions anonymous_read;
    anonymous_read.allow_anonymous_read = true;
    auto anonymous_mw = nas::nas_basic_auth_middleware(std::make_shared<nas::NasAuthService>(memory), anonymous_read);
    req->reset();
    resp->reset();
    req->set_method(yuan::net::http::HttpMethod::propfind_);
    check(nas::NasPermissionService::required_for_webdav_request(*req) == nas::NasPermission::read,
          "PROPFIND should map to read permission");
    check(anonymous_mw->process(req, resp) == yuan::net::http::MiddlewareResult::next,
          "NAS auth middleware should allow anonymous read when configured");

#if YUAN_NAS_HAS_REDIS
    nas::NasRedisConfig redis;
    redis.key_prefix = "test:nas";
    nas::NasRedisMetadataStore store(redis);
    check(store.key("users") == "test:nas:users", "redis key prefix should be normalized");
    check(store.user_key("u1") == "test:nas:user:u1", "redis user key should match schema");
    check(store.share_by_name_key("public") == "test:nas:share_by_name:public", "redis share name index key should match schema");
    check(store.dead_property_key("s1", "/docs/readme.txt") == "test:nas:webdav:prop:s1:docs/readme.txt",
          "redis dead property key should normalize paths");
    check(!store.available(), "redis metadata store without init should not be available");
#endif

    if (failed != 0) {
        std::cerr << "nas core tests failed=" << failed << '\n';
        return 1;
    }
    std::cout << "nas core tests passed\n";
    return 0;
}
