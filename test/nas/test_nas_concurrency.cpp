#include "nas/nas.h"

#include "base/utils/base64.h"
#include "http/http_service.h"

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

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override
        {
            return yuan::server::nas::NasPermission::none;
        }
        bool set_permissions(std::string_view, std::string_view, yuan::server::nas::NasPermission) override { return true; }
        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override
        {
            return {};
        }
        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override { return true; }
        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override { return true; }

        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            locks[lock.token] = lock;
            return true;
        }
        bool try_create_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            locks[lock.token] = lock;
            return true;
        }
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view token) const override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            auto it = locks.find(std::string(token));
            return it == locks.end() ? std::nullopt : std::optional<yuan::server::nas::NasWebDavLockRecord>(it->second);
        }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view share_id,
                                                                               std::string_view path) const override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            std::vector<yuan::server::nas::NasWebDavLockRecord> out;
            const std::string target = normalize_lock_path(path);
            const auto now_ms = now_unix_ms();
            for (const auto &[_, lock] : locks) {
                if (lock.share_id != share_id) continue;
                if (lock.expires_at_unix_ms > 0 && lock.expires_at_unix_ms <= now_ms) continue;
                const std::string locked = normalize_lock_path(lock.path);
                if (locked == target || (lock.depth == "infinity" && target.rfind(locked + "/", 0) == 0)) {
                    out.push_back(lock);
                }
            }
            return out;
        }
        bool remove_webdav_lock(std::string_view token) override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            return locks.erase(std::string(token)) != 0;
        }
        std::size_t prune_expired_webdav_locks() override
        {
            std::lock_guard<std::mutex> lock_guard(mutex_);
            const auto now_ms = now_unix_ms();
            std::size_t removed = 0;
            for (auto it = locks.begin(); it != locks.end();) {
                if (it->second.expires_at_unix_ms > 0 && it->second.expires_at_unix_ms <= now_ms) {
                    it = locks.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            return removed;
        }

        bool append_audit_event(const yuan::server::nas::NasAuditEvent &) override { return true; }
        std::vector<yuan::server::nas::NasAuditEvent> list_audit_events(std::size_t) const override { return {}; }
        bool upsert_admin_session(const yuan::server::nas::NasAdminSession &) override { return true; }
        std::vector<yuan::server::nas::NasAdminSession> list_admin_sessions(std::size_t) const override { return {}; }

        std::unordered_map<std::string, yuan::server::nas::NasUser> users;

    private:
        static std::int64_t now_unix_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        static std::string normalize_lock_path(std::string_view path)
        {
            std::string normalized(path.empty() ? "/" : path);
            if (normalized.front() != '/') normalized.insert(normalized.begin(), '/');
            while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
            return normalized;
        }

        mutable std::mutex mutex_;
        std::unordered_map<std::string, yuan::server::nas::NasWebDavLockRecord> locks;
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

    std::optional<std::string> header_value(const std::string &response, const std::string &header_name)
    {
        const auto header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return std::nullopt;
        }
        const auto lower = to_lower_ascii(response.substr(0, header_end));
        const std::string needle = to_lower_ascii(header_name) + ":";
        const auto pos = lower.find(needle);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        const auto line_end = lower.find("\r\n", pos);
        if (line_end == std::string::npos) {
            return std::nullopt;
        }
        std::string value = response.substr(pos + needle.size(), line_end - (pos + needle.size()));
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    int status_code(const std::string &response)
    {
        const auto first_line_end = response.find("\r\n");
        const auto first_line = response.substr(0, first_line_end);
        const auto first_space = first_line.find(' ');
        if (first_space == std::string::npos || first_space + 4 > first_line.size()) {
            return 0;
        }
        return std::stoi(first_line.substr(first_space + 1, 3));
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
    const auto root = std::filesystem::temp_directory_path() / "yuan_nas_webdav_concurrency";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "stress", ec);

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
    http_cfg.enable_ssl = false;
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

    std::atomic<int> worker_failures{ 0 };
    std::vector<std::thread> workers;
    constexpr int kThreads = 6;
    constexpr int kOpsPerThread = 8;

    for (int tid = 0; tid < kThreads; ++tid) {
        workers.emplace_back([&, tid]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                const std::string base = "t" + std::to_string(tid) + "-" + std::to_string(i);
                const std::string src = "/dav/public/stress/" + base + ".txt";
                const std::string cp = "/dav/public/stress/" + base + ".copy.txt";
                const std::string mv = "/dav/public/stress/" + base + ".moved.txt";
                const std::string body = "payload-" + base;

                const std::string put_req =
                    "PUT " + src + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n\r\n" + body;
                const auto put_resp = roundtrip(port, put_req);
                const int put_code = status_code(put_resp);
                if (!(put_code == 201 || put_code == 204)) {
                    ++worker_failures;
                    continue;
                }

                const std::string get_req =
                    "GET " + src + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Connection: close\r\n\r\n";
                const auto get_resp = roundtrip(port, get_req);
                if (status_code(get_resp) != 200 || get_resp.find(body) == std::string::npos) {
                    ++worker_failures;
                    continue;
                }

                const std::string copy_req =
                    "COPY " + src + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Destination: " + cp + "\r\n"
                    "Overwrite: T\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                const int copy_code = status_code(roundtrip(port, copy_req));
                if (!(copy_code == 201 || copy_code == 204)) {
                    ++worker_failures;
                    continue;
                }

                const std::string move_req =
                    "MOVE " + cp + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Destination: " + mv + "\r\n"
                    "Overwrite: T\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                const int move_code = status_code(roundtrip(port, move_req));
                if (!(move_code == 201 || move_code == 204)) {
                    ++worker_failures;
                    continue;
                }

                const std::string del_src_req =
                    "DELETE " + src + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                const int del_src_code = status_code(roundtrip(port, del_src_req));
                if (del_src_code != 204) {
                    ++worker_failures;
                    continue;
                }

                const std::string del_mv_req =
                    "DELETE " + mv + " HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Authorization: " + auth + "\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                const int del_mv_code = status_code(roundtrip(port, del_mv_req));
                if (del_mv_code != 204) {
                    ++worker_failures;
                    continue;
                }
            }
        });
    }
    for (auto &t : workers) {
        t.join();
    }

    check(worker_failures.load() == 0, "parallel WebDAV CRUD/COPY/MOVE workflow should succeed");

    const std::string lock_path = "/dav/public/stress/lock-target.txt";
    const std::string seed_body = "seed";
    const std::string seed_put =
        "PUT " + lock_path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(seed_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + seed_body;
    check(status_code(roundtrip(port, seed_put)) >= 200, "seed file should be created before lock contention");

    const std::string lock_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:exclusive/></D:lockscope>"
        "<D:locktype><D:write/></D:locktype><D:owner>alice</D:owner></D:lockinfo>";
    const std::string lock_req =
        "LOCK " + lock_path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Depth: 0\r\n"
        "Timeout: Second-10\r\n"
        "Content-Type: application/xml\r\n"
        "Content-Length: " + std::to_string(lock_body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + lock_body;
    const auto lock_resp = roundtrip(port, lock_req);
    check(status_code(lock_resp) == 200, "LOCK should succeed in concurrency test");
    const auto token_opt = header_value(lock_resp, "Lock-Token");
    check(token_opt.has_value(), "LOCK should return Lock-Token header");

    const std::string body_without_token = "blocked";
    const std::string put_without_token =
        "PUT " + lock_path + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: " + auth + "\r\n"
        "Content-Length: " + std::to_string(body_without_token.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body_without_token;
    check(status_code(roundtrip(port, put_without_token)) == 423,
          "PUT without lock token should be rejected during contention");

    if (token_opt) {
        const std::string token = *token_opt;
        const std::string body_with_token = "allowed";
        const std::string put_with_token =
            "PUT " + lock_path + " HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Authorization: " + auth + "\r\n"
            "If: <" + lock_path + "> (" + token + ")\r\n"
            "Content-Length: " + std::to_string(body_with_token.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body_with_token;
        const int put_with_token_code = status_code(roundtrip(port, put_with_token));
        check(put_with_token_code == 201 || put_with_token_code == 204,
              "PUT with valid lock token should pass during contention");

        const std::string unlock_req =
            "UNLOCK " + lock_path + " HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Authorization: " + auth + "\r\n"
            "Lock-Token: " + token + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        check(status_code(roundtrip(port, unlock_req)) == 204, "UNLOCK should succeed in concurrency test");
    }

    service.stop();
    std::filesystem::remove_all(root, ec);

#ifdef _WIN32
    WSACleanup();
#endif

    if (failed != 0) {
        std::cerr << "nas concurrency failed=" << failed << '\n';
        return 1;
    }
    std::cout << "nas concurrency passed\n";
    return 0;
}
