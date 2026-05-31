#include "nas/nas_service.h"

#include "base/utils/base64.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

#include <chrono>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

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

    class MemoryMetadataStore final : public yuan::server::nas::NasMetadataStore
    {
    public:
        bool available() const override { return true; }
        std::optional<yuan::server::nas::NasUser> find_user_by_name(std::string_view username) const override
        {
            auto it = users.find(std::string(username));
            return it == users.end() ? std::nullopt : std::optional<yuan::server::nas::NasUser>(it->second);
        }
        std::vector<yuan::server::nas::NasUser> list_users() const override
        {
            std::vector<yuan::server::nas::NasUser> out;
            for (const auto &[_, user] : users) {
                out.push_back(user);
            }
            return out;
        }
        bool upsert_user(const yuan::server::nas::NasUser &user) override
        {
            users[user.username] = user;
            return true;
        }
        std::optional<yuan::server::nas::NasShare> find_share_by_name(std::string_view name) const override
        {
            auto it = shares.find(std::string(name));
            return it == shares.end() ? std::nullopt : std::optional<yuan::server::nas::NasShare>(it->second);
        }
        std::vector<yuan::server::nas::NasShare> list_shares() const override
        {
            std::vector<yuan::server::nas::NasShare> out;
            for (const auto &[_, share] : shares) {
                out.push_back(share);
            }
            return out;
        }
        bool upsert_share(const yuan::server::nas::NasShare &share) override
        {
            shares[share.name] = share;
            return true;
        }
        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override { return yuan::server::nas::NasPermission::none; }
        bool set_permissions(std::string_view share_id,
                             std::string_view subject,
                             yuan::server::nas::NasPermission permissions) override
        {
            for (auto &[_, share] : shares) {
                if (share.id == share_id) {
                    share.subject_permissions[std::string(subject)] = permissions;
                    return true;
                }
            }
            return false;
        }
        bool remove_permissions(std::string_view share_id, std::string_view subject) override
        {
            for (auto &[_, share] : shares) {
                if (share.id == share_id) {
                    share.subject_permissions.erase(std::string(subject));
                    return true;
                }
            }
            return false;
        }
        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override { return {}; }
        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override { return true; }
        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override { return true; }
        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            locks[lock.token] = lock;
            return true;
        }
        bool try_create_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            return upsert_webdav_lock(lock);
        }
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view token) const override
        {
            auto it = locks.find(std::string(token));
            return it == locks.end() ? std::nullopt : std::optional<yuan::server::nas::NasWebDavLockRecord>(it->second);
        }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view, std::string_view) const override { return {}; }
        bool remove_webdav_lock(std::string_view token) override { return locks.erase(std::string(token)) != 0; }
        std::size_t prune_expired_webdav_locks() override { return 0; }
        bool append_audit_event(const yuan::server::nas::NasAuditEvent &event) override
        {
            if (fail_audit_append) {
                return false;
            }
            audit.push_back(event);
            while (audit.size() > audit_max_events) {
                audit.erase(audit.begin());
            }
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

        std::unordered_map<std::string, yuan::server::nas::NasUser> users;
        std::unordered_map<std::string, yuan::server::nas::NasShare> shares;
        std::unordered_map<std::string, yuan::server::nas::NasWebDavLockRecord> locks;
        std::vector<yuan::server::nas::NasAuditEvent> audit;
        std::unordered_map<std::string, yuan::server::nas::NasAdminSession> sessions;
        bool fail_audit_append = false;
        std::size_t audit_max_events = 1000;
    };

    void close_socket(socket_t s)
    {
        if (s == kInvalidSocket) return;
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }

    uint16_t reserve_tcp_port()
    {
        socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(listener);
            return 0;
        }
        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    std::string to_lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string roundtrip(uint16_t port, const std::string &request, int timeout_ms = 5000)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket) return {};
#ifdef _WIN32
        const DWORD socket_timeout_ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 5000;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char *>(&socket_timeout_ms), sizeof(socket_timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = timeout_ms > 0 ? timeout_ms / 1000 : 5;
        tv.tv_usec = timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0;
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return {};
        }
#ifdef _WIN32
        ::send(s, request.data(), static_cast<int>(request.size()), 0);
#else
        ::send(s, request.data(), request.size(), 0);
#endif
        std::string out;
        char buf[4096];
        while (true) {
#ifdef _WIN32
            const int rc = ::recv(s, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t rc = ::recv(s, buf, sizeof(buf), 0);
#endif
            if (rc <= 0) break;
            out.append(buf, static_cast<std::size_t>(rc));
            const auto header_end = out.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                const auto lower_headers = to_lower_ascii(out.substr(0, header_end));
                const auto len_pos = lower_headers.find("content-length:");
                if (len_pos != std::string::npos && len_pos < header_end) {
                    const auto len_end = lower_headers.find("\r\n", len_pos);
                    const auto len_text = lower_headers.substr(len_pos + 15, len_end - (len_pos + 15));
                    const auto body_len = static_cast<std::size_t>(std::stoull(len_text));
                    if (out.size() >= header_end + 4 + body_len) {
                        break;
                    }
                } else if (lower_headers.find("connection: keep-alive") == std::string::npos) {
                    break;
                }
            }
        }
        close_socket(s);
        return out;
    }
}

int main()
{
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    namespace nas = yuan::server::nas;

    const uint16_t port = reserve_tcp_port();
    check(port != 0, "should reserve port");

    const auto root = std::filesystem::temp_directory_path() / "yuan_nas_service_smoke";
    const auto root_reloaded = std::filesystem::temp_directory_path() / "yuan_nas_service_smoke_reloaded";
    const auto root_admin = std::filesystem::temp_directory_path() / "yuan_nas_service_smoke_admin";
    const auto root_smb = std::filesystem::temp_directory_path() / "yuan_nas_service_smoke_smb";
    const auto audit_path = root / "nas-audit.jsonl";
    const auto admin_console_path = root / "admin-console.html";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(root_reloaded, ec);
    std::filesystem::remove_all(root_admin, ec);
    std::filesystem::remove_all(root_smb, ec);
    std::filesystem::create_directories(root, ec);
    std::filesystem::create_directories(root_reloaded, ec);
    std::filesystem::create_directories(root_admin, ec);
    std::filesystem::create_directories(root_smb, ec);
    {
        std::ofstream out(admin_console_path, std::ios::binary | std::ios::trunc);
        out << "<!doctype html><html><head><title>Custom Admin</title></head>"
            << "<body><h1>Custom NAS Admin UI</h1></body></html>";
    }

    auto metadata = std::make_shared<MemoryMetadataStore>();
    nas::NasUser user;
    user.id = "u1";
    user.username = "alice";
    user.password_hash = nas::NasAuthService::hash_password_for_config("secret", "salt");
    user.admin = true;
    metadata->upsert_user(user);
    nas::NasUser reader;
    reader.id = "u2";
    reader.username = "bob";
    reader.password_hash = nas::NasAuthService::hash_password_for_config("reader", "salt");
    metadata->upsert_user(reader);

    const auto config_path = root / "nas.json";
    {
        std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
        out << "{"
            << "\"production_mode\":true,"
            << "\"port\":" << port << ","
            << "\"http\":{\"enable_ssl\":false,\"enable_keep_alive\":false},"
            << "\"nas\":{"
            << "\"webdav_mount\":\"/dav\"," 
            << "\"admin_console_path\":\"admin-console.html\"," 
            << "\"redis\":{\"enabled\":false},"
            << "\"audit\":{\"file_enabled\":true,\"file_path\":\"" << audit_path.generic_string() << "\",\"max_events\":3},"
            << "\"users\":[{\"id\":\"u1\",\"username\":\"alice\",\"password_hash\":\""
            << user.password_hash << "\",\"admin\":true},"
            << "{\"id\":\"u2\",\"username\":\"bob\",\"password_hash\":\""
            << reader.password_hash << "\"}],"
            << "\"shares\":[{\"id\":\"s1\",\"name\":\"public\",\"root_path\":\""
            << root.generic_string() << "\",\"default_permissions\":[\"read\",\"write\",\"remove\"],"
            << "\"subject_permissions\":{\"u2\":[\"read\"]}}]"
            << "}}";
    }
    auto loaded = yuan::server::load_nas_service_config(config_path);
    check(loaded.has_value(), "nas service config should load from JSON");
    check(loaded && loaded->production_mode, "nas service config should parse production mode");
    check(loaded && loaded->port == port, "nas service config should parse port");
    check(loaded && loaded->nas.shares.size() == 1, "nas service config should parse shares");
    check(loaded && loaded->nas.shares.front().subject_permissions.count("u2") == 1,
          "nas service config should parse share subject permissions");
    check(loaded && loaded->bootstrap_users.size() == 2, "nas service config should parse bootstrap users");
    check(loaded && loaded->nas.audit.file_path == audit_path.generic_string(),
          "nas service config should parse audit file path");
    check(loaded && loaded->nas.admin_console_path == admin_console_path.lexically_normal().string(),
          "nas service config should parse and resolve admin console path");

    if (!loaded) {
        return 1;
    }
    loaded->metadata = metadata;
    metadata->audit_max_events = loaded->nas.audit.max_events;
    {
        auto weak_metadata = std::make_shared<MemoryMetadataStore>();
        auto weak_config = *loaded;
        weak_config.port = reserve_tcp_port();
        weak_config.metadata = weak_metadata;
        weak_config.bootstrap_users.clear();
        nas::NasUser weak_admin;
        weak_admin.id = "weak-admin";
        weak_admin.username = "weak-admin";
        weak_admin.password_hash = "plain:weak";
        weak_admin.admin = true;
        weak_config.bootstrap_users.push_back(weak_admin);
        yuan::server::NasService weak_service(std::move(weak_config));
        check(!weak_service.init(), "production NAS service init should fail closed on weak password hashes");
    }

    yuan::server::NasService service(*loaded);
    check(service.init(), "nas service init should succeed");
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto health_resp = roundtrip(port,
        "GET /nas/health HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(health_resp.find("200") != std::string::npos, "nas health should return 200");
    check(health_resp.find("\"metadata_available\":true") != std::string::npos, "nas health should report metadata");
    check(health_resp.find("\"share_count\":1") != std::string::npos, "nas health should report share count");
    check(health_resp.find("\"smb_enabled\":false") != std::string::npos, "nas health should report smb disabled by default");
    check(health_resp.find("\"smb_require_signing\":false") != std::string::npos,
          "nas health should report smb signing disabled by default");

    const std::string admin_auth = "Basic " + yuan::base::util::base64_encode("alice:secret");
    const std::string reader_auth = "Basic " + yuan::base::util::base64_encode("bob:reader");
    nas::NasAuthService auth_check(metadata);
    check(auth_check.authenticate_basic_header(reader_auth).authenticated, "reader basic auth should authenticate");
    const auto admin_unauth_resp = roundtrip(port,
        "GET /nas/admin/shares HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(admin_unauth_resp.find("401") != std::string::npos, "admin shares should require auth");
    const auto admin_forbidden_resp = roundtrip(port,
        "GET /nas/admin/shares HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + reader_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(admin_forbidden_resp.find("403") != std::string::npos, "admin shares should require admin user");
    const auto admin_shares_resp = roundtrip(port,
        "GET /nas/admin/shares HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(admin_shares_resp.find("200") != std::string::npos, "admin shares should return 200");
    check(admin_shares_resp.find("\"name\":\"public\"") != std::string::npos,
          "admin shares should include configured share");
    const auto admin_users_resp = roundtrip(port,
        "GET /nas/admin/users HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(admin_users_resp.find("200") != std::string::npos, "admin users should return 200");
    check(admin_users_resp.find("\"username\":\"alice\"") != std::string::npos,
          "admin users should include admin user");
    check(admin_users_resp.find("\"username\":\"bob\"") != std::string::npos,
          "admin users should include regular user");
    check(admin_users_resp.find("password_hash") == std::string::npos,
          "admin users should not expose password hashes");
    const auto admin_console_resp = roundtrip(port,
        "GET /nas/admin HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(admin_console_resp.find("200") != std::string::npos, "admin console should return 200");
    check(admin_console_resp.find("Content-Type: text/html; charset=utf-8") != std::string::npos ||
              admin_console_resp.find("content-type: text/html; charset=utf-8") != std::string::npos,
          "admin console should return html content type");
    check(admin_console_resp.find("Custom Admin") != std::string::npos,
          "admin console should include configured title");
    check(admin_console_resp.find("Custom NAS Admin UI") != std::string::npos,
          "admin console should serve configured admin_console_path content");

    const std::string auth = admin_auth;
    const std::string new_user_body =
        "{\"id\":\"u3\",\"username\":\"charlie\",\"password\":\"change-me\",\"salt\":\"salt\",\"enabled\":true,\"admin\":false}";
    const auto new_user_resp = roundtrip(port,
        "POST /nas/admin/users HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(new_user_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + new_user_body);
    check(new_user_resp.find("200") != std::string::npos, "admin users POST should return 200");
    auto charlie = metadata->find_user_by_name("charlie");
    check(charlie && !charlie->admin && charlie->enabled, "admin users POST should persist user");
    check(!metadata->audit.empty() && metadata->audit.back().action == "user.upsert",
          "admin users POST should write audit event");

    const std::string disable_user_body =
        "{\"username\":\"charlie\",\"enabled\":false}";
    const auto disable_user_resp = roundtrip(port,
        "POST /nas/admin/users HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(disable_user_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + disable_user_body);
    check(disable_user_resp.find("200") != std::string::npos,
          "admin users POST should allow partial user updates");
    charlie = metadata->find_user_by_name("charlie");
    check(charlie && !charlie->enabled && !charlie->password_hash.empty(),
          "partial user update should preserve password hash and disable user");

    const std::string new_share_body =
        "{\"id\":\"s2\",\"name\":\"media\",\"root_path\":\"" + root_admin.generic_string() +
        "\",\"default_permissions\":[\"read\",\"write\",\"remove\"]}";
    const auto new_share_resp = roundtrip(port,
        "POST /nas/admin/shares HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(new_share_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + new_share_body);
    check(new_share_resp.find("200") != std::string::npos, "admin shares POST should return 200");
    check(metadata->find_share_by_name("media").has_value(), "admin shares POST should persist share");
    check(!metadata->audit.empty() && metadata->audit.back().action == "share.upsert",
          "admin shares POST should write audit event");

    const std::string permission_body =
        "{\"share\":\"media\",\"username\":\"bob\",\"permissions\":[\"read\"]}";
    const auto permission_resp = roundtrip(port,
        "POST /nas/admin/permissions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(permission_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + permission_body);
    check(permission_resp.find("200") != std::string::npos, "admin permissions POST should return 200");
    auto media_acl = metadata->find_share_by_name("media");
    check(media_acl && media_acl->subject_permissions.count("u2") == 1,
          "admin permissions POST should persist per-user share permissions");
    check(media_acl && yuan::server::nas::has_permission(media_acl->subject_permissions["u2"], nas::NasPermission::read) &&
              !yuan::server::nas::has_permission(media_acl->subject_permissions["u2"], nas::NasPermission::write),
          "admin permissions POST should store exact permission mask");
    check(!metadata->audit.empty() && metadata->audit.back().action == "share.permission.set",
          "admin permissions POST should write audit event");

    const auto acl_shares_resp = roundtrip(port,
        "GET /nas/admin/shares HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(acl_shares_resp.find("\"subject_permissions\"") != std::string::npos,
          "admin shares should include subject permissions");
    check(acl_shares_resp.find("\"username\":\"bob\"") != std::string::npos,
          "admin shares should resolve ACL subjects to usernames");

    const auto media_reader_put_resp = roundtrip(port,
        "PUT /dav/media/reader-denied.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + reader_auth + "\r\n"
        "Content-Length: 1\r\n"
        "Connection: close\r\n\r\nx");
    check(media_reader_put_resp.find("403") != std::string::npos,
          "share subject permissions should override defaults for WebDAV writes");

    const std::string clear_permission_body =
        "{\"share\":\"media\",\"username\":\"bob\",\"clear\":true}";
    const auto clear_permission_resp = roundtrip(port,
        "POST /nas/admin/permissions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(clear_permission_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + clear_permission_body);
    check(clear_permission_resp.find("200") != std::string::npos,
          "admin permissions POST should clear per-user share permissions");
    media_acl = metadata->find_share_by_name("media");
    check(media_acl && media_acl->subject_permissions.count("u2") == 0,
          "cleared share permissions should fall back to defaults");

    const std::string media_body = "media";
    const auto media_put_resp = roundtrip(port,
        "PUT /dav/media/from-admin.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Length: " + std::to_string(media_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + media_body);
    check(media_put_resp.find("201") != std::string::npos || media_put_resp.find("204") != std::string::npos,
          "admin-created share should be mounted for WebDAV");
    check(std::filesystem::exists(root_admin / "from-admin.txt"), "admin-created share should write to new root");

    const auto quota_resp = roundtrip(port,
        "GET /nas/admin/quota HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(quota_resp.find("200") != std::string::npos, "admin quota should return 200");
    check(quota_resp.find("\"name\":\"media\"") != std::string::npos,
          "admin quota should include admin-created share");
    check(quota_resp.find("\"used_bytes\":5") != std::string::npos,
          "admin quota should report share used bytes");
    const auto health_actions_resp = roundtrip(port,
        "GET /nas/admin/health/actions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(health_actions_resp.find("200") != std::string::npos,
          "admin health actions should return 200");
    check(health_actions_resp.find("\"probe\"") != std::string::npos,
          "admin health actions should list probe action");
    const std::string health_probe_body = "{\"action\":\"probe\"}";
    const auto health_probe_resp = roundtrip(port,
        "POST /nas/admin/health/actions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(health_probe_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + health_probe_body);
    check(health_probe_resp.find("200") != std::string::npos,
          "admin health probe should return 200");
    check(health_probe_resp.find("\"action\":\"probe\"") != std::string::npos,
          "admin health probe should echo action");
    check(health_probe_resp.find("\"metadata_available\":true") != std::string::npos,
          "admin health probe should include health status");
    check(!metadata->audit.empty() && metadata->audit.back().action == "service.health_probe",
          "admin health probe should write audit event");
    const auto activity_resp = roundtrip(port,
        "GET /nas/admin/activity HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(activity_resp.find("200") != std::string::npos, "admin activity should return 200");
    check(activity_resp.find("\"started\":true") != std::string::npos,
          "admin activity should report started service");
    check(activity_resp.find("\"share_count\":2") != std::string::npos,
          "admin activity should report updated share count");
    const auto readiness_resp = roundtrip(port,
        "GET /nas/admin/readiness HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(readiness_resp.find("200") != std::string::npos, "admin readiness should return 200");
    check(readiness_resp.find("\"ready\":true") != std::string::npos,
          "admin readiness should report sell-ready state");
    check(readiness_resp.find("\"blocker_count\":0") != std::string::npos,
          "admin readiness should report no blockers");
    check(readiness_resp.find("\"warning_count\":0") != std::string::npos,
          "admin readiness should report no warnings by default");
    check(readiness_resp.find("\"production_mode\":true") != std::string::npos,
          "admin readiness should include production mode");
    nas::NasUser legacy_user;
    legacy_user.id = "legacy";
    legacy_user.username = "legacy";
    legacy_user.password_hash = "plain:legacy";
    metadata->upsert_user(legacy_user);
    const auto weak_readiness_resp = roundtrip(port,
        "GET /nas/admin/readiness HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(weak_readiness_resp.find("503") != std::string::npos,
          "production readiness should fail on weak password hashes");
    check(weak_readiness_resp.find("\"weak_password_hash\"") != std::string::npos,
          "production readiness should name weak password hash blocker");
    legacy_user.enabled = false;
    metadata->upsert_user(legacy_user);
    const auto sessions_resp = roundtrip(port,
        "GET /nas/admin/sessions HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "User-Agent: nas-service-test\r\n"
        "Connection: close\r\n\r\n");
    check(sessions_resp.find("200") != std::string::npos, "admin sessions should return 200");
    check(sessions_resp.find("\"username\":\"alice\"") != std::string::npos,
          "admin sessions should include admin user");
    check(sessions_resp.find("\"last_path\":\"/nas/admin/sessions\"") != std::string::npos,
          "admin sessions should track last admin path");
    check(!metadata->sessions.empty(), "admin requests should persist session records");
    const auto audit_resp = roundtrip(port,
        "GET /nas/admin/audit HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(audit_resp.find("200") != std::string::npos, "admin audit should return 200");
    check(audit_resp.find("\"action\":\"share.permission.clear\"") != std::string::npos,
          "admin audit should include permission clear changes");
    check(audit_resp.find("\"action\":\"share.permission.set\"") != std::string::npos,
          "admin audit should include permission changes");

    metadata->fail_audit_append = true;
    const std::string fallback_user_body =
        "{\"id\":\"u4\",\"username\":\"dana\",\"password\":\"fallback\",\"salt\":\"salt\",\"enabled\":true,\"admin\":false}";
    const auto fallback_user_resp = roundtrip(port,
        "POST /nas/admin/users HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(fallback_user_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + fallback_user_body);
    check(fallback_user_resp.find("200") != std::string::npos,
          "admin users POST should succeed when audit metadata append fails");
    check(std::filesystem::exists(audit_path), "audit file fallback should be created");
    const auto fallback_audit_resp = roundtrip(port,
        "GET /nas/admin/audit HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + admin_auth + "\r\n"
        "Connection: close\r\n\r\n");
    check(fallback_audit_resp.find("\"target\":\"dana\"") != std::string::npos,
          "admin audit should include file fallback events");
    check(metadata->audit.size() == 3,
          "metadata audit retention should keep only max_events entries");
    check(fallback_audit_resp.find("\"count\":3") != std::string::npos,
          "admin audit endpoint should apply max_events retention");
    metadata->fail_audit_append = false;

    const std::string body = "service";
    const auto resp = roundtrip(port,
        "PUT /dav/public/service.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body);
    check(resp.find("201") != std::string::npos || resp.find("204") != std::string::npos,
          "nas service should mount authenticated WebDAV");
    check(std::filesystem::exists(root / "service.txt"), "nas service should write into share root");

    const auto reload_config_path = root / "nas-reload.json";
    {
        std::ofstream out(reload_config_path, std::ios::binary | std::ios::trunc);
        out << "{"
            << "\"port\":" << port << ","
            << "\"http\":{\"enable_ssl\":false,\"enable_keep_alive\":false},"
            << "\"nas\":{"
            << "\"webdav_mount\":\"/dav\","
            << "\"redis\":{\"enabled\":false},"
            << "\"users\":[{\"id\":\"u1\",\"username\":\"alice\",\"password_hash\":\""
            << user.password_hash << "\",\"admin\":true},"
            << "{\"id\":\"u2\",\"username\":\"bob\",\"password_hash\":\""
            << reader.password_hash << "\"}],"
            << "\"shares\":[{\"id\":\"s1\",\"name\":\"public\",\"root_path\":\""
            << root_reloaded.generic_string() << "\",\"default_permissions\":[\"read\",\"write\",\"remove\"]}]"
            << "}}";
    }
    check(service.reload_from_file(reload_config_path), "nas service should reload from JSON");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto reload_health_resp = roundtrip(port,
        "GET /nas/health HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(reload_health_resp.find("200") != std::string::npos, "nas health should return 200 after reload");
    check(reload_health_resp.find("\"share_count\":1") != std::string::npos,
          "nas health should report share count after reload");

    const std::string reload_body = "reloaded";
    const auto reload_resp = roundtrip(port,
        "PUT /dav/public/reloaded.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(reload_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + reload_body);
    check(reload_resp.find("201") != std::string::npos || reload_resp.find("204") != std::string::npos,
          "nas service should accept WebDAV after reload");
    check(std::filesystem::exists(root_reloaded / "reloaded.txt"), "nas service reload should update share root");

    const uint16_t smb_port = reserve_tcp_port();
    check(smb_port != 0, "should reserve smb port");
    const auto smb_config_path = root / "nas-smb-reload.json";
    {
        std::ofstream out(smb_config_path, std::ios::binary | std::ios::trunc);
        out << "{"
            << "\"port\":" << port << ","
            << "\"http\":{\"enable_ssl\":false,\"enable_keep_alive\":false},"
            << "\"smb\":{\"enabled\":true,\"port\":" << smb_port << ",\"require_signing\":true},"
            << "\"nas\":{"
            << "\"webdav_mount\":\"/dav\","
            << "\"redis\":{\"enabled\":false},"
            << "\"users\":[{\"id\":\"u1\",\"username\":\"alice\",\"password_hash\":\""
            << user.password_hash << "\",\"admin\":true}],"
            << "\"shares\":[{\"id\":\"s1\",\"name\":\"public\",\"root_path\":\""
            << root_smb.generic_string() << "\",\"default_permissions\":[\"read\",\"write\",\"remove\"]}]"
            << "}}";
    }

    check(service.reload_from_file(smb_config_path), "nas service should reload with smb enabled");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto smb_health_resp = roundtrip(port,
        "GET /nas/health HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(smb_health_resp.find("\"smb_enabled\":true") != std::string::npos,
          "nas health should report smb enabled after reload");
    check(smb_health_resp.find("\"smb_port\":" + std::to_string(smb_port)) != std::string::npos,
          "nas health should expose smb port");
    check(smb_health_resp.find("\"smb_require_signing\":true") != std::string::npos,
          "nas health should expose smb signing requirement");

    socket_t smb_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    bool smb_connect_ok = false;
    if (smb_socket != kInvalidSocket) {
        sockaddr_in smb_addr{};
        smb_addr.sin_family = AF_INET;
        smb_addr.sin_port = htons(smb_port);
        smb_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        smb_connect_ok = (::connect(smb_socket, reinterpret_cast<const sockaddr *>(&smb_addr), sizeof(smb_addr)) == 0);
    }
    close_socket(smb_socket);
    check(smb_connect_ok, "nas service should accept smb tcp connection when enabled");

    service.stop();
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove_all(root_reloaded, ec);
    std::filesystem::remove_all(root_admin, ec);
    std::filesystem::remove_all(root_smb, ec);
#ifdef _WIN32
    WSACleanup();
#endif
    if (failed != 0) {
        std::cerr << "nas service failed=" << failed << '\n';
        return 1;
    }
    std::cout << "nas service passed\n";
    return 0;
}
