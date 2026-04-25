#include "nas/nas.h"

#include "base/utils/base64.h"
#include "http_service.h"

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

#include <cctype>
#include <algorithm>
#include <chrono>
#include <filesystem>
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
        std::optional<yuan::server::nas::NasShare> find_share_by_name(std::string_view) const override { return std::nullopt; }
        std::vector<yuan::server::nas::NasShare> list_shares() const override { return {}; }
        bool upsert_share(const yuan::server::nas::NasShare &) override { return true; }
        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override { return yuan::server::nas::NasPermission::none; }
        bool set_permissions(std::string_view, std::string_view, yuan::server::nas::NasPermission) override { return true; }
        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override { return {}; }
        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override { return true; }
        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override { return true; }
        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            locks[lock.token] = lock;
            return true;
        }
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view token) const override
        {
            auto it = locks.find(std::string(token));
            return it == locks.end() ? std::nullopt
                                     : std::optional<yuan::server::nas::NasWebDavLockRecord>(it->second);
        }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view share_id,
                                                                              std::string_view path) const override
        {
            std::vector<yuan::server::nas::NasWebDavLockRecord> out;
            const std::string target = normalize_lock_path(path);
            for (const auto &[_, lock] : locks) {
                if (lock.share_id != share_id) continue;
                const std::string locked = normalize_lock_path(lock.path);
                if (locked == target || (lock.depth == "infinity" && target.rfind(locked + "/", 0) == 0)) {
                    out.push_back(lock);
                }
            }
            return out;
        }
        bool remove_webdav_lock(std::string_view token) override { return locks.erase(std::string(token)) != 0; }
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

        std::unordered_map<std::string, yuan::server::nas::NasUser> users;
        std::unordered_map<std::string, yuan::server::nas::NasWebDavLockRecord> locks;
        std::vector<yuan::server::nas::NasAuditEvent> audit;
        std::unordered_map<std::string, yuan::server::nas::NasAdminSession> sessions;

    private:
        static std::string normalize_lock_path(std::string_view path)
        {
            std::string normalized(path.empty() ? "/" : path);
            if (normalized.front() != '/') normalized.insert(normalized.begin(), '/');
            while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
            return normalized;
        }
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

    socket_t connect_loopback(uint16_t port)
    {
        socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s == kInvalidSocket) return kInvalidSocket;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(s);
            return kInvalidSocket;
        }
        return s;
    }

    bool send_all(socket_t s, const std::string &data)
    {
        std::size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            const int rc = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t rc = ::send(s, data.data() + sent, data.size() - sent, 0);
#endif
            if (rc <= 0) return false;
            sent += static_cast<std::size_t>(rc);
        }
        return true;
    }

    std::string recv_response(socket_t s)
    {
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
                const auto len_pos = out.find("content-length:");
                if (len_pos != std::string::npos && len_pos < header_end) {
                    const auto len_end = out.find("\r\n", len_pos);
                    const auto len_text = out.substr(len_pos + 15, len_end - (len_pos + 15));
                    const auto body_len = static_cast<std::size_t>(std::stoull(len_text));
                    if (out.size() >= header_end + 4 + body_len) {
                        break;
                    }
                } else if (out.find("connection: keep-alive") == std::string::npos) {
                    break;
                }
            }
            if (out.size() > 4 * 1024 * 1024) break;
        }
        return out;
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

    std::string http_roundtrip(uint16_t port, const std::string &request)
    {
        socket_t s = connect_loopback(port);
        if (s == kInvalidSocket) return {};
        send_all(s, request);
        auto response = recv_response(s);
        close_socket(s);
        return response;
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
    const auto root = std::filesystem::temp_directory_path() / "yuan_nas_webdav_integration";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    auto metadata = std::make_shared<MemoryMetadataStore>();
    nas::NasUser user;
    user.id = "u1";
    user.username = "alice";
    user.password_hash = nas::NasAuthService::hash_password_for_config("secret", "salt");
    metadata->upsert_user(user);

    nas::NasShare share;
    share.id = "s1";
    share.name = "public";
    share.root_path = root.string();
    share.default_permissions = nas::NasPermission::read | nas::NasPermission::write | nas::NasPermission::remove;

    yuan::net::http::HttpServerConfig http_cfg;
    http_cfg.enable_keep_alive = false;
    yuan::server::HttpService service(port, http_cfg);
    check(service.init(), "http service should init");
    nas::NasConfig cfg;
    cfg.webdav_mount = "/dav";
    cfg.shares.push_back(share);
    nas::mount_nas_webdav(service.server(), cfg, metadata);
    service.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const std::string auth = "Basic " + yuan::base::util::base64_encode("alice:secret");
    const std::string body = "hello nas";
    const std::string put =
        "PUT /dav/public/docs/a.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    const auto put_resp = http_roundtrip(port, put);
    check(put_resp.find("201") != std::string::npos || put_resp.find("204") != std::string::npos,
          "authorized PUT should create file");

    const std::string propfind =
        "PROPFIND /dav/public/docs/a.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Depth: 0\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto propfind_resp = http_roundtrip(port, propfind);
    check(propfind_resp.find("207") != std::string::npos, "authorized PROPFIND should return multistatus");

    const std::string large_body(256 * 1024, 'z');
    const std::string large_put =
        "PUT /dav/public/docs/large.bin HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(large_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + large_body;
    const auto large_put_resp = http_roundtrip(port, large_put);
    check(large_put_resp.find("201") != std::string::npos || large_put_resp.find("204") != std::string::npos,
          "authorized large PUT should create file");

    const std::string large_get =
        "GET /dav/public/docs/large.bin HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Connection: close\r\n\r\n";
    const auto large_get_resp = http_roundtrip(port, large_get);
    check(large_get_resp.find("200") != std::string::npos, "authorized large GET should return 200");
    check(large_get_resp.find("content-length: " + std::to_string(large_body.size())) != std::string::npos,
          "large GET should advertise full content length");
    check(large_get_resp.size() >= large_body.size(), "large GET should stream response body");

    const std::string mkcol =
        "MKCOL /dav/public/projects HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto mkcol_resp = http_roundtrip(port, mkcol);
    check(mkcol_resp.find("201") != std::string::npos, "authorized MKCOL should create collection");

    const std::string copy_req =
        "COPY /dav/public/docs/a.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Destination: /dav/public/docs/copy.txt\r\n"
        "Overwrite: F\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto copy_resp = http_roundtrip(port, copy_req);
    check(copy_resp.find("201") != std::string::npos, "authorized COPY should create destination");

    const std::string move_req =
        "MOVE /dav/public/docs/copy.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Destination: /dav/public/docs/moved.txt\r\n"
        "Overwrite: T\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto move_resp = http_roundtrip(port, move_req);
    check(move_resp.find("201") != std::string::npos || move_resp.find("204") != std::string::npos,
          "authorized MOVE should move destination");

    const std::string lock_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>"
        "<D:locktype><D:write/></D:locktype><D:owner>alice</D:owner></D:lockinfo>";
    const std::string lock_req =
        "LOCK /dav/public/docs/moved.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Depth: 0\r\n"
        "Content-Type: application/xml\r\n"
        "Content-Length: " + std::to_string(lock_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + lock_body;
    const auto lock_resp = http_roundtrip(port, lock_req);
    check(lock_resp.find("200") != std::string::npos, "authorized LOCK should return 200");
    std::string lock_token;
    const auto lt_pos = lock_resp.find("lock-token:");
    if (lt_pos != std::string::npos) {
        const auto lt_end = lock_resp.find("\r\n", lt_pos);
        lock_token = lock_resp.substr(lt_pos + 11, lt_end - (lt_pos + 11));
        while (!lock_token.empty() && std::isspace(static_cast<unsigned char>(lock_token.front()))) {
            lock_token.erase(lock_token.begin());
        }
    }
    check(!lock_token.empty(), "LOCK should return lock-token header");

    const std::string locked_put =
        "PUT /dav/public/docs/moved.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: 6\r\n"
        "Connection: close\r\n\r\nlocked";
    const auto locked_put_resp = http_roundtrip(port, locked_put);
    check(locked_put_resp.find("423") != std::string::npos, "PUT without lock token should be locked");

    const std::string unlock_req =
        "UNLOCK /dav/public/docs/moved.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Lock-Token: " + lock_token + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto unlock_resp = http_roundtrip(port, unlock_req);
    check(unlock_resp.find("204") != std::string::npos, "authorized UNLOCK should return 204");

    const std::string delete_req =
        "DELETE /dav/public/docs/moved.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const auto delete_resp = http_roundtrip(port, delete_req);
    check(delete_resp.find("204") != std::string::npos, "authorized DELETE should remove file");

    const std::string unauth =
        "PUT /dav/public/docs/b.txt HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 1\r\n"
        "Connection: close\r\n\r\nx";
    const auto unauth_resp = http_roundtrip(port, unauth);
    check(unauth_resp.find("401") != std::string::npos, "unauthorized PUT should return 401");

    service.stop();
    std::filesystem::remove_all(root, ec);
#ifdef _WIN32
    WSACleanup();
#endif
    if (failed != 0) {
        std::cerr << "nas webdav integration failed=" << failed << '\n';
        return 1;
    }
    std::cout << "nas webdav integration passed\n";
    return 0;
}
