#include "ssh.h"

#include "net/acceptor/acceptor_factory.h"
#include "net/socket/socket.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace yuan::net::ssh;

namespace
{
#ifdef _WIN32
    using SocketFd = SOCKET;
    constexpr SocketFd kInvalidSocketFd = INVALID_SOCKET;
#else
    using SocketFd = int;
    constexpr SocketFd kInvalidSocketFd = -1;
#endif

    void close_socket(SocketFd fd)
    {
        if (fd == kInvalidSocketFd) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void set_socket_recv_timeout(SocketFd fd, int timeout_ms)
    {
        if (fd == kInvalidSocketFd || timeout_ms <= 0) {
            return;
        }
#ifdef _WIN32
        const DWORD value = static_cast<DWORD>(timeout_ms);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char *>(&value),
                   sizeof(value));
#else
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    class RemoteForwardPasswordHandler final : public SshHandler
    {
    public:
        bool on_channel_open(SshSession *, const std::string & channel_type, SshChannel *) override
        {
            return channel_type == SSH_CHANNEL_SESSION;
        }

        SshAuthResult on_authenticate(SshSession *,
                                      const std::string & username,
                                      const std::string & method,
                                      const SshAuthCredentials & credentials) override
        {
            if (method != "password") {
                return SshAuthResult::FAILURE;
            }
            if (username != "cli") {
                return SshAuthResult::FAILURE;
            }
            return credentials.password == "cli-pass"
                       ? SshAuthResult::SUCCESS
                       : SshAuthResult::FAILURE;
        }

        bool on_exec_request(SshSession *, SshChannel *, const std::string &) override
        {
            return true;
        }

        bool enable_builtin_exec_bridge() const override
        {
            return true;
        }

        uint16_t on_tcpip_forward(SshSession *session,
                                  const std::string &bind_addr,
                                  uint16_t bind_port) override
        {
            if (!session || !session->server()) {
                return 0;
            }
            auto *runtime = session->server()->runtime();
            if (!runtime || !runtime->event_loop()) {
                return 0;
            }

            auto socket = std::make_unique<yuan::net::Socket>(bind_addr, bind_port);
            if (!socket->valid()) {
                return 0;
            }
#ifdef _WIN32
            socket->set_reuse(true, true);
#else
            socket->set_reuse(true);
#endif
            socket->set_none_block(true);
            if (!socket->bind()) {
                return 0;
            }

            const uint16_t allocated_port = static_cast<uint16_t>(socket->get_local_address().get_port());
            auto acceptor = std::shared_ptr<yuan::net::StreamAcceptor>(
                yuan::net::create_stream_acceptor(socket.release()),
                [](yuan::net::StreamAcceptor *ptr) {
                    if (ptr) {
                        ptr->close();
                        delete ptr;
                    }
                });
            if (!acceptor || !acceptor->listen()) {
                return 0;
            }
            acceptor->set_event_handler(runtime->event_loop());
            acceptor->update_channel();

            std::lock_guard<std::mutex> lock(mutex_);
            listeners_[key(bind_addr, allocated_port)] = acceptor;
            return allocated_port;
        }

        void on_cancel_tcpip_forward(SshSession *, const std::string &bind_addr, uint16_t bind_port) override
        {
            std::shared_ptr<yuan::net::StreamAcceptor> acceptor;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = listeners_.find(key(bind_addr, bind_port));
                if (it != listeners_.end()) {
                    acceptor = std::move(it->second);
                    listeners_.erase(it);
                }
            }
            if (acceptor) {
                acceptor->close();
            }
        }

        std::shared_ptr<yuan::net::StreamAcceptor> on_forwarded_tcpip_listener(
            SshSession *,
            const std::string &bind_addr,
            uint16_t bind_port) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = listeners_.find(key(bind_addr, bind_port));
            if (it == listeners_.end()) {
                return {};
            }
            auto acceptor = std::move(it->second);
            listeners_.erase(it);
            return acceptor;
        }

    private:
        static std::string key(const std::string &bind_addr, uint16_t bind_port)
        {
            return bind_addr + ":" + std::to_string(bind_port);
        }

        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<yuan::net::StreamAcceptor>> listeners_;
    };

    std::string shell_quote(const std::string & value)
    {
#ifdef _WIN32
        return "\"" + value + "\"";
#else
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out += "'";
        return out;
#endif
    }

    int run_command(const std::string & command)
    {
        return std::system(command.c_str());
    }

    std::filesystem::path find_release_cli_binary()
    {
        const auto cwd = std::filesystem::current_path();
        const auto from_build = cwd / "release" / "ssh" / "release_ssh_cli";
        if (std::filesystem::exists(from_build)) {
            return from_build;
        }
        const auto from_repo = cwd / "build" / "release" / "ssh" / "release_ssh_cli";
        if (std::filesystem::exists(from_repo)) {
            return from_repo;
        }
        return {};
    }

    SocketFd connect_once(const std::string & host, uint16_t port)
    {
        SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocketFd) {
            return kInvalidSocketFd;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            close_socket(fd);
            return kInvalidSocketFd;
        }

        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(fd);
            return kInvalidSocketFd;
        }
        return fd;
    }
}

int main()
{
    const char * smoke_env = std::getenv("YUAN_RUN_RELEASE_SSH_CLI_REMOTE_FORWARD_SMOKE");
    if (!smoke_env || std::string(smoke_env) != "1") {
        std::cout << "release_ssh_cli remote-forward smoke skipped (set YUAN_RUN_RELEASE_SSH_CLI_REMOTE_FORWARD_SMOKE=1)." << std::endl;
        return 0;
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif

    const auto cli_bin = find_release_cli_binary();
    if (cli_bin.empty()) {
        std::cout << "release_ssh_cli binary not found; skipping remote-forward smoke." << std::endl;
        return 0;
    }

    const auto smoke_dir = std::filesystem::current_path() / ".tmp_release_ssh_cli_remote_forward_smoke";
    std::error_code ec;
    std::filesystem::remove_all(smoke_dir, ec);
    std::filesystem::create_directories(smoke_dir, ec);
    if (ec) {
        std::cerr << "failed to create smoke dir: " << ec.message() << std::endl;
        return 1;
    }

    const auto host_key = smoke_dir / "ssh_host_ed25519_key";
    const auto known_hosts = smoke_dir / "known_hosts";
    const auto cli_out = smoke_dir / "cli_out.txt";
    const auto cli_err = smoke_dir / "cli_err.txt";

    SshHostKeyProvider host_key_provider;
    if (!host_key_provider.generate_key(SshHostKeyType::ED25519, host_key.string())) {
        std::cerr << "failed to generate host key" << std::endl;
        return 1;
    }

    constexpr int server_port = 22432;
    constexpr int forward_port = 22433;
    constexpr int target_port = 22434;

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.auth_methods = { "password" };
    config.enable_builtin_terminal_handler = true;
    config.enable_port_forwarding = true;

    RemoteForwardPasswordHandler handler;
    auto server = std::make_unique<SshServer>(config);
    server->set_handler(&handler);
    if (!server->init(server_port)) {
        std::cerr << "failed to init ssh server" << std::endl;
        return 1;
    }

    std::atomic<bool> echo_ready{ false };
    std::atomic<bool> echo_ok{ false };
    std::thread echo_thread([&]() {
        SocketFd listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocketFd) {
            return;
        }

        int on = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(target_port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return;
        }
        if (listen(listener, 1) != 0) {
            close_socket(listener);
            return;
        }
        echo_ready.store(true, std::memory_order_relaxed);

        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        SocketFd conn = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len);
        close_socket(listener);
        if (conn == kInvalidSocketFd) {
            return;
        }

        set_socket_recv_timeout(conn, 5000);

        std::array<char, 256> buf{};
        const int n = recv(conn, buf.data(), static_cast<int>(buf.size()), 0);
        if (n > 0) {
            const std::string got(buf.data(), buf.data() + n);
            const std::string want = "rf-smoke\n";
            if (got == want) {
                const std::string reply = "rf-smoke-reply\n";
                (void)send(conn, reply.data(), static_cast<int>(reply.size()), 0);
                echo_ok.store(true, std::memory_order_relaxed);
            }
        }
        close_socket(conn);
    });

    std::thread server_thread([&server]() {
        server->serve();
    });

    for (int i = 0; i < 30 && !echo_ready.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!echo_ready.load(std::memory_order_relaxed)) {
        std::cerr << "echo target did not become ready" << std::endl;
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (echo_thread.joinable()) {
            echo_thread.join();
        }
        return 1;
    }

    std::string cli_cmd = shell_quote(cli_bin.string()) +
                          " --host 127.0.0.1 --port " + std::to_string(server_port) +
                          " --user cli --password cli-pass" +
                          " --strict-host-key-checking accept-new" +
                          " --known-hosts " + shell_quote(known_hosts.string()) +
                          " -R 127.0.0.1:" + std::to_string(forward_port) + ":127.0.0.1:" + std::to_string(target_port) +
                          " --command " + shell_quote("sleep 8") +
                          " > " + shell_quote(cli_out.string()) +
                          " 2> " + shell_quote(cli_err.string());
#ifndef _WIN32
    cli_cmd = "timeout 20s " + cli_cmd;
#endif

    std::atomic<int> cli_status{ -1 };
    std::atomic<bool> cli_done{ false };
    std::thread cli_thread([&]() {
        cli_status.store(run_command(cli_cmd), std::memory_order_relaxed);
        cli_done.store(true, std::memory_order_relaxed);
    });

    bool forwarded_exchange_ok = false;
    for (int i = 0; i < 150 && !cli_done.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        SocketFd probe = connect_once("127.0.0.1", static_cast<uint16_t>(forward_port));
        if (probe == kInvalidSocketFd) {
            continue;
        }
        set_socket_recv_timeout(probe, 1000);

        const std::string line = "rf-smoke\n";
        if (send(probe, line.data(), static_cast<int>(line.size()), 0) <= 0) {
            close_socket(probe);
            continue;
        }

        std::array<char, 256> buf{};
        const int n = recv(probe, buf.data(), static_cast<int>(buf.size()), 0);
        close_socket(probe);
        if (n > 0) {
            const std::string got(buf.data(), buf.data() + n);
            if (got == "rf-smoke-reply\n") {
                forwarded_exchange_ok = true;
                break;
            }
        }
    }

    if (!cli_done.load(std::memory_order_relaxed)) {
        std::cerr << "release_ssh_cli remote-forward command timed out in smoke harness" << std::endl;
    }

    if (cli_thread.joinable()) {
        cli_thread.join();
    }

    if (!echo_ok.load(std::memory_order_relaxed)) {
        SocketFd wake = connect_once("127.0.0.1", static_cast<uint16_t>(target_port));
        if (wake != kInvalidSocketFd) {
            close_socket(wake);
        }
    }

    server->stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    if (echo_thread.joinable()) {
        echo_thread.join();
    }

    if (cli_status.load(std::memory_order_relaxed) != 0) {
        std::cerr << "release_ssh_cli remote-forward command failed with status " << cli_status.load() << std::endl;
        return 1;
    }
    if (!echo_ok.load(std::memory_order_relaxed) || !forwarded_exchange_ok) {
        std::cerr << "release_ssh_cli remote-forward data path failed" << std::endl;
        return 1;
    }

    std::cout << "release_ssh_cli remote-forward smoke passed." << std::endl;
    return 0;
}
