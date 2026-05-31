#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    int g_failed = 0;

    void check(bool cond, const char *msg)
    {
        if (!cond) {
            ++g_failed;
            std::cerr << "[FAIL] " << msg << '\n';
        } else {
            std::cout << "[PASS] " << msg << '\n';
        }
    }

    std::string mini_nginx_bin()
    {
#ifdef MINI_NGINX_BIN_PATH
        return std::string(MINI_NGINX_BIN_PATH);
#else
        return "mini_nginx";
#endif
    }

#ifndef _WIN32
    struct RunResult
    {
        int exit_code = -1;
        bool timed_out = false;
        std::string output;
    };

    RunResult run_and_capture(const std::string &bin,
                              const std::string &cfg,
                              std::chrono::milliseconds timeout)
    {
        int pipefd[2] = { -1, -1 };
        if (pipe(pipefd) != 0) {
            return {};
        }

        const pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            execl(bin.c_str(), bin.c_str(), cfg.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }

        close(pipefd[1]);
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

        RunResult result;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        char buffer[1024];

        while (true) {
            const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n > 0) {
                result.output.append(buffer, static_cast<size_t>(n));
            }

            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = 128 + WTERMSIG(status);
                }
                break;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                kill(pid, SIGTERM);
                waitpid(pid, &status, 0);
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = 128 + WTERMSIG(status);
                }
                break;
            }

            usleep(10 * 1000);
        }

        while (true) {
            const ssize_t n = read(pipefd[0], buffer, sizeof(buffer));
            if (n > 0) {
                result.output.append(buffer, static_cast<size_t>(n));
            } else {
                break;
            }
        }
        close(pipefd[0]);

        return result;
    }

    bool send_all(int fd, const std::string &data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    std::string recv_head(int fd)
    {
        std::string out;
        char buf[1024];
        while (out.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            out.append(buf, static_cast<size_t>(n));
            if (out.size() > 16 * 1024) {
                break;
            }
        }
        return out;
    }

    int connect_loopback(uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    bool run_rate_limit_probe(const std::string &bin, const std::filesystem::path &cfg_path)
    {
        const pid_t pid = fork();
        if (pid == 0) {
            execl(bin.c_str(), bin.c_str(), cfg_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int status_429 = 0;
        int status_non_429 = 0;
        for (int i = 0; i < 6; ++i) {
            int fd = connect_loopback(18083);
            if (fd < 0) {
                continue;
            }
            const std::string req =
                "GET /hello.txt HTTP/1.1\r\n"
                "Host: 127.0.0.1:18083\r\n"
                "Connection: close\r\n\r\n";
            if (send_all(fd, req)) {
                const std::string resp = recv_head(fd);
                if (resp.find(" 429 ") != std::string::npos) {
                    ++status_429;
                } else {
                    ++status_non_429;
                }
            }
            ::close(fd);
        }

        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return status_429 > 0 && status_non_429 > 0;
    }

    bool run_max_connections_probe(const std::string &bin, const std::filesystem::path &cfg_path)
    {
        const pid_t pid = fork();
        if (pid == 0) {
            execl(bin.c_str(), bin.c_str(), cfg_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        int fd1 = connect_loopback(18084);
        int fd2 = connect_loopback(18084);
        int fd3 = connect_loopback(18084);

        bool got_reject = false;
        auto try_probe = [&](int fd) {
            if (fd < 0) {
                return;
            }
            const std::string req =
                "GET /hello.txt HTTP/1.1\r\n"
                "Host: 127.0.0.1:18084\r\n"
                "Connection: keep-alive\r\n\r\n";
            if (!send_all(fd, req)) {
                got_reject = true;
                return;
            }
            const std::string resp = recv_head(fd);
            if (resp.find(" 503 ") != std::string::npos || resp.empty()) {
                got_reject = true;
            }
        };

        try_probe(fd1);
        try_probe(fd2);
        try_probe(fd3);

        if (fd1 >= 0) {
            ::close(fd1);
        }
        if (fd2 >= 0) {
            ::close(fd2);
        }
        if (fd3 >= 0) {
            ::close(fd3);
        }

        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return got_reject;
    }

    bool run_inflight_limit_probe(const std::string &bin, const std::filesystem::path &cfg_path)
    {
        const pid_t pid = fork();
        if (pid == 0) {
            execl(bin.c_str(), bin.c_str(), cfg_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::atomic<int> status_429{0};
        auto one_req = [&]() {
            int fd = connect_loopback(18085);
            if (fd < 0) {
                return;
            }
            const std::string req =
                "GET /api/slow HTTP/1.1\r\n"
                "Host: 127.0.0.1:18085\r\n"
                "Connection: close\r\n\r\n";
            if (send_all(fd, req)) {
                const std::string resp = recv_head(fd);
                if (resp.find(" 429 ") != std::string::npos) {
                    status_429.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ::close(fd);
        };

        std::thread t1(one_req);
        std::thread t2(one_req);
        t1.join();
        t2.join();

        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return status_429.load(std::memory_order_relaxed) > 0;
    }

    bool write_reload_limit_config(const std::filesystem::path &cfg_path, int max_connections)
    {
        std::ofstream out(cfg_path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }

        out << "{\n"
            << "  \"server\": {\n"
            << "    \"listen\": 18086,\n"
            << "    \"server_name\": \"mini-nginx-reload-limits\",\n"
            << "    \"max_connections\": " << max_connections << ",\n"
            << "    \"max_connections_per_ip\": 0,\n"
            << "    \"max_inflight_requests_per_ip\": 0\n"
            << "  },\n"
            << "  \"reload_check_interval_ms\": 200,\n"
            << "  \"upstreams\": {\n"
            << "    \"api_backend\": {\n"
            << "      \"servers\": [\n"
            << "        { \"host\": \"127.0.0.1\", \"port\": 9001, \"weight\": 1 }\n"
            << "      ]\n"
            << "    }\n"
            << "  },\n"
            << "  \"routes\": [\n"
            << "    {\n"
            << "      \"path\": \"/api/\",\n"
            << "      \"proxy_pass\": \"api_backend\"\n"
            << "    }\n"
            << "  ],\n"
            << "  \"static\": [\n"
            << "    {\n"
            << "      \"location\": \"/\",\n"
            << "      \"root\": \"release/mini_nginx/www\",\n"
            << "      \"auto_index\": true\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
        out.flush();
        return out.good();
    }

    bool run_reload_limit_probe(const std::string &bin, const std::filesystem::path &cfg_path)
    {
        if (!write_reload_limit_config(cfg_path, 0)) {
            return false;
        }

        const pid_t pid = fork();
        if (pid == 0) {
            execl(bin.c_str(), bin.c_str(), cfg_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        const std::string req =
            "GET /hello.txt HTTP/1.1\r\n"
            "Host: 127.0.0.1:18086\r\n"
            "Connection: keep-alive\r\n\r\n";

        auto read_ok = [&](int fd) {
            if (fd < 0) {
                return false;
            }
            if (!send_all(fd, req)) {
                return false;
            }
            const std::string resp = recv_head(fd);
            return !resp.empty() && resp.find(" 503 ") == std::string::npos;
        };

        int fd1 = connect_loopback(18086);
        int fd2 = connect_loopback(18086);
        const bool baseline_ok = read_ok(fd1) && read_ok(fd2);
        if (fd1 >= 0) {
            ::close(fd1);
        }
        if (fd2 >= 0) {
            ::close(fd2);
        }

        if (!write_reload_limit_config(cfg_path, 1)) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1600));

        int fd3 = connect_loopback(18086);
        int fd4 = connect_loopback(18086);

        bool got_reject = false;
        auto probe_reject = [&](int fd) {
            if (fd < 0) {
                got_reject = true;
                return;
            }
            if (!send_all(fd, req)) {
                got_reject = true;
                return;
            }
            const std::string resp = recv_head(fd);
            if (resp.empty() || resp.find(" 503 ") != std::string::npos) {
                got_reject = true;
            }
        };

        probe_reject(fd3);
        probe_reject(fd4);

        if (fd3 >= 0) {
            ::close(fd3);
        }
        if (fd4 >= 0) {
            ::close(fd4);
        }

        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        return baseline_ok && got_reject;
    }

    bool run_edge_features_probe(const std::string &bin, const std::filesystem::path &cfg_path)
    {
        const pid_t pid = fork();
        if (pid == 0) {
            execl(bin.c_str(), bin.c_str(), cfg_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0) {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(600));

        auto request = [](const std::string &raw) {
            int fd = connect_loopback(18087);
            if (fd < 0) {
                return std::string();
            }
            std::string resp;
            if (send_all(fd, raw)) {
                resp = recv_head(fd);
            }
            ::close(fd);
            return resp;
        };

        const std::string health = request(
            "GET /healthz HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Connection: close\r\n\r\n");
        const std::string redirect = request(
            "GET /old-static/index.html HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Connection: close\r\n\r\n");
        const std::string rejected = request(
            "TRACE / HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Connection: close\r\n\r\n");
        const std::string cached_static = request(
            "GET /index.html HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Connection: close\r\n\r\n");
        const std::string custom_404 = request(
            "GET /not-found-for-error-page HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Connection: close\r\n\r\n");
        const std::string nogzip_static = request(
            "GET /nogzip/index.html HTTP/1.1\r\n"
            "Host: 127.0.0.1:18087\r\n"
            "Accept-Encoding: gzip\r\n"
            "Connection: close\r\n\r\n");

        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);

        return health.find(" 200 ") != std::string::npos &&
               health.find("X-Test-Header: edge") != std::string::npos &&
               redirect.find(" 301 ") != std::string::npos &&
               redirect.find("Location: /static/index.html") != std::string::npos &&
               rejected.find(" 405 ") != std::string::npos &&
               rejected.find("Allow:") != std::string::npos &&
               cached_static.find(" 200 ") != std::string::npos &&
               cached_static.find("Cache-Control: public, max-age=60") != std::string::npos &&
               cached_static.find("Expires:") != std::string::npos &&
               cached_static.find("X-Static-Header: asset") != std::string::npos &&
               custom_404.find(" 404 ") != std::string::npos &&
               nogzip_static.find(" 200 ") != std::string::npos &&
               nogzip_static.find("Content-Encoding:") == std::string::npos;
    }
#endif
}

int main()
{
#ifdef _WIN32
    std::cout << "mini_nginx config test skipped on Windows\n";
    return 0;
#else
    const auto work_dir = std::filesystem::current_path();
    const auto cfg_legacy = work_dir / "mini_nginx_legacy_only_test.json";
    const auto cfg_bad_port = work_dir / "mini_nginx_bad_port_test.json";
    const auto cfg_new = work_dir / "mini_nginx_new_format_only_test.json";
    const auto cfg_rate_limit = work_dir / "mini_nginx_rate_limit_test.json";
    const auto cfg_max_connections = work_dir / "mini_nginx_max_connections_test.json";
    const auto cfg_inflight = work_dir / "mini_nginx_inflight_limit_test.json";
    const auto cfg_reload_limits = work_dir / "mini_nginx_reload_limits_test.json";
    const auto cfg_static_only = work_dir / "mini_nginx_static_only_test.json";

    {
        std::ofstream out(cfg_legacy, std::ios::binary | std::ios::trunc);
        out << R"({
  "proxies": [
    {
      "root": "/api/",
      "target": [["127.0.0.1", 9001]]
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_bad_port, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": { "listen": 18082, "server_name": "mini-nginx-invalid-port" },
  "upstreams": {
    "bad_backend": {
      "servers": [
        { "host": "127.0.0.1", "port": 70000, "weight": 1 }
      ]
    }
  },
  "routes": [
    {
      "path": "/api/",
      "proxy_pass": "bad_backend"
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_new, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": {
    "listen": 18081,
    "server_name": "mini-nginx-test",
    "thread_pool_size": 1,
    "enable_keep_alive": true,
    "enable_cors": false,
    "enable_http2": false,
    "max_body_size": 1048576
  },
  "access_log": {
    "enabled": true,
    "json": false,
    "path": "mini_nginx_access_test.log"
  },
  "reload_check_interval_ms": 250,
  "upstreams": {
    "api_backend": {
      "balance": "round_robin",
      "servers": [
        { "host": "127.0.0.1", "port": 9001, "weight": 1 }
      ]
    }
  },
  "routes": [
    {
      "path": "/api/",
      "proxy_pass": "api_backend",
      "strip_prefix": false
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_rate_limit, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": {
    "listen": 18083,
    "server_name": "mini-nginx-rate-limit"
  },
  "rate_limit": {
    "enabled": true,
    "requests_per_second": 1,
    "burst": 0
  },
  "upstreams": {
    "api_backend": {
      "connect_timeout": 1500,
      "max_retries": 0,
      "servers": [
        { "host": "127.0.0.1", "port": 1, "weight": 1 }
      ]
    }
  },
  "routes": [
    {
      "path": "/api/",
      "proxy_pass": "api_backend"
    }
  ],
  "static": [
    {
      "location": "/",
      "root": "release/mini_nginx/www",
      "auto_index": true,
      "cache_control": "public, max-age=60",
      "expires": "1h",
      "headers": {
        "X-Static-Header": "asset"
      }
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_max_connections, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": {
    "listen": 18084,
    "server_name": "mini-nginx-max-connections",
    "max_connections": 2,
    "max_connections_per_ip": 2,
    "max_inflight_requests_per_ip": 0
  },
  "upstreams": {
    "api_backend": {
      "servers": [
        { "host": "127.0.0.1", "port": 9001, "weight": 1 }
      ]
    }
  },
  "routes": [
    {
      "path": "/api/",
      "proxy_pass": "api_backend"
    }
  ],
  "static": [
    {
      "location": "/",
      "root": "release/mini_nginx/www",
      "auto_index": true
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_inflight, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": {
    "listen": 18085,
    "server_name": "mini-nginx-inflight-limit",
    "max_connections": 0,
    "max_connections_per_ip": 0,
    "max_inflight_requests_per_ip": 1
  },
  "rate_limit": {
    "enabled": false
  },
  "upstreams": {
    "api_backend": {
      "servers": [
        { "host": "127.0.0.1", "port": 9001, "weight": 1 }
      ]
    }
  },
  "routes": [
    {
      "path": "/api/",
      "proxy_pass": "api_backend"
    }
  ],
  "static": [
    {
      "location": "/",
      "root": "release/mini_nginx/www",
      "auto_index": true
    }
  ]
})";
    }

    {
        std::ofstream out(cfg_static_only, std::ios::binary | std::ios::trunc);
        out << R"({
  "server": {
    "listen": 18087,
    "server_name": "mini-nginx-static-only",
    "enable_ssl": false,
    "backlog": 256,
    "use_iocp": true,
    "iocp_worker_count": 2,
    "allowed_methods": ["GET", "HEAD"],
    "client_max_body_size": "2m",
    "keepalive_timeout": "30s",
    "send_timeout": "5s"
  },
  "health": {
    "enabled": true,
    "path": "/healthz",
    "json": true
  },
  "headers": {
    "add": {
      "X-Test-Header": "edge"
    }
  },
  "redirects": [
    {
      "from": "/old-static",
      "to": "/static",
      "code": 301,
      "prefix": true,
      "preserve_path": true
    }
  ],
  "access_log": {
    "enabled": true,
    "path": "tmp/mini_nginx_static_only_access.log"
  },
  "static": [
    {
      "location": "/",
      "root": "release/mini_nginx/www",
      "auto_index": true,
      "autoindex": true,
      "gzip": true,
      "gzip_static": true,
      "sendfile": true,
      "gzip_min_length": "1k",
      "gzip_types": ["text/*", "application/json", "application/x-test"],
      "default_type": "application/octet-stream",
      "types": {
        "application/x-test": ["foo"]
      },
      "cache_control": "public, max-age=60",
      "expires": "1h",
      "headers": {
        "X-Static-Header": "asset"
      },
      "error_page": {
        "404": "/404.html"
      }
    },
    {
      "location": "/nogzip",
      "root": "release/mini_nginx/www",
      "autoindex": false,
      "gzip": false,
      "gzip_static": false
    }
  ],
  "routes": [
    {
      "location": "/direct/",
      "proxy_pass": "http://127.0.0.1:1/backend",
      "proxy_connect_timeout": "250ms",
      "proxy_read_timeout": "1s",
      "proxy_send_timeout": "1s"
    }
  ]
})";
    }

    const std::string bin = mini_nginx_bin();

    const auto legacy_result = run_and_capture(bin, cfg_legacy.string(), std::chrono::milliseconds(1000));
    check(!legacy_result.timed_out, "legacy-only config should fail quickly");
    check(legacy_result.exit_code != 0, "legacy-only config should return non-zero");
    check(legacy_result.output.find("config 'routes' is required") != std::string::npos,
          "legacy-only config should report missing routes");

    const auto bad_port_result = run_and_capture(bin, cfg_bad_port.string(), std::chrono::milliseconds(1000));
    check(!bad_port_result.timed_out, "bad-port config should fail quickly");
    check(bad_port_result.exit_code != 0, "bad-port config should return non-zero");
    check(bad_port_result.output.find("port must be in range [1, 65535]") != std::string::npos,
          "bad-port config should report port range error");

    const auto new_result = run_and_capture(bin, cfg_new.string(), std::chrono::milliseconds(1500));
    check(new_result.timed_out, "new-format config should start and keep running");

    const auto static_only_result = run_and_capture(bin, cfg_static_only.string(), std::chrono::milliseconds(1500));
    check(static_only_result.timed_out, "static-only config should start and keep running");
    check(run_edge_features_probe(bin, cfg_static_only),
          "edge features should handle health, redirect, headers, and method allow-list");

    check(run_rate_limit_probe(bin, cfg_rate_limit),
          "rate-limit config should produce 429 under burst traffic");

    check(run_max_connections_probe(bin, cfg_max_connections),
          "max-connections config should reject excess connections");

    check(run_inflight_limit_probe(bin, cfg_inflight),
          "inflight-limit config should produce 429 when exceeded");

    check(run_reload_limit_probe(bin, cfg_reload_limits),
          "hot-reload should apply max_connections changes without restart");

    std::error_code ec;
    std::filesystem::remove(cfg_legacy, ec);
    std::filesystem::remove(cfg_bad_port, ec);
    std::filesystem::remove(cfg_new, ec);
    std::filesystem::remove(cfg_rate_limit, ec);
    std::filesystem::remove(cfg_max_connections, ec);
    std::filesystem::remove(cfg_inflight, ec);
    std::filesystem::remove(cfg_reload_limits, ec);
    std::filesystem::remove(cfg_static_only, ec);
    std::filesystem::remove(work_dir / "mini_nginx_access_test.log", ec);
    std::filesystem::remove(work_dir / "tmp" / "mini_nginx_static_only_access.log", ec);
    std::filesystem::remove(work_dir / "tmp", ec);

    if (g_failed != 0) {
        std::cerr << "mini_nginx config tests failed=" << g_failed << '\n';
        return 1;
    }
    std::cout << "mini_nginx config tests passed\n";
    return 0;
#endif
}
