#include "ssh.h"

#include "net/acceptor/acceptor_factory.h"
#include "net/socket/socket.h"

#include <array>
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

    class MixedForwardPasswordHandler final : public SshHandler
    {
    public:
        bool on_channel_open(SshSession *, const std::string & channel_type, SshChannel *) override
        {
            return channel_type == SSH_CHANNEL_SESSION || channel_type == SSH_CHANNEL_DIRECT_TCPIP;
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

        bool on_direct_tcpip(SshSession *, SshChannel *, const std::string &, uint16_t) override
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

    std::thread launch_echo_server(int port,
                                   const std::string & expect,
                                   const std::string & reply,
                                   std::atomic<bool> & ready,
                                   std::atomic<bool> & ok)
    {
        return std::thread([port, expect, reply, &ready, &ok]() {
            SocketFd listener = socket(AF_INET, SOCK_STREAM, 0);
            if (listener == kInvalidSocketFd) {
                return;
            }

            int on = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on), sizeof(on));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                close_socket(listener);
                return;
            }
            if (listen(listener, 1) != 0) {
                close_socket(listener);
                return;
            }
            ready.store(true, std::memory_order_relaxed);

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
                if (got == expect) {
                    (void)send(conn, reply.data(), static_cast<int>(reply.size()), 0);
                    ok.store(true, std::memory_order_relaxed);
                }
            }
            close_socket(conn);
        });
    }
}

int main()
{
    const char * soak_env = std::getenv("YUAN_RUN_RELEASE_SSH_CLI_MIXED_FORWARD_SOAK");
    if (!soak_env || std::string(soak_env) != "1") {
        std::cout << "release_ssh_cli mixed-forward soak skipped (set YUAN_RUN_RELEASE_SSH_CLI_MIXED_FORWARD_SOAK=1)." << std::endl;
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
        std::cout << "release_ssh_cli binary not found; skipping mixed-forward soak." << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    }

    const auto soak_dir = std::filesystem::current_path() / ".tmp_release_ssh_cli_mixed_forward_soak";
    std::error_code ec;
    std::filesystem::remove_all(soak_dir, ec);
    std::filesystem::create_directories(soak_dir, ec);
    if (ec) {
        std::cerr << "failed to create soak dir: " << ec.message() << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    const auto host_key = soak_dir / "ssh_host_ed25519_key";
    const auto known_hosts = soak_dir / "known_hosts";
    const auto cli_out = soak_dir / "cli_out.txt";
    const auto cli_err = soak_dir / "cli_err.txt";

    SshHostKeyProvider host_key_provider;
    if (!host_key_provider.generate_key(SshHostKeyType::ED25519, host_key.string())) {
        std::cerr << "failed to generate host key" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    constexpr int server_port = 22441;
    constexpr int local_port = 22442;
    constexpr int dynamic_port = 22443;
    constexpr int remote_bind_port = 22444;
    constexpr int target_local_port = 22445;
    constexpr int target_remote_port = 22446;
    constexpr int target_dynamic_port = 22447;

    SshServerConfig config;
    config.host_key_paths = { host_key.string() };
    config.auth_methods = { "password" };
    config.enable_builtin_terminal_handler = true;
    config.enable_port_forwarding = true;

    MixedForwardPasswordHandler handler;
    auto server = std::make_unique<SshServer>(config);
    server->set_handler(&handler);
    if (!server->init(server_port)) {
        std::cerr << "failed to init ssh server" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::atomic<bool> local_ready{ false };
    std::atomic<bool> remote_ready{ false };
    std::atomic<bool> dynamic_ready{ false };
    std::atomic<bool> local_ok{ false };
    std::atomic<bool> remote_ok{ false };
    std::atomic<bool> dynamic_ok{ false };

    auto local_echo = launch_echo_server(target_local_port, "mix-l\n", "mix-l-reply\n", local_ready, local_ok);
    auto remote_echo = launch_echo_server(target_remote_port, "mix-r\n", "mix-r-reply\n", remote_ready, remote_ok);
    auto dynamic_echo = launch_echo_server(target_dynamic_port, "mix-d\n", "mix-d-reply\n", dynamic_ready, dynamic_ok);

    std::thread server_thread([&server]() {
        server->serve();
    });

    for (int i = 0; i < 40; ++i) {
        if (local_ready.load(std::memory_order_relaxed) &&
            remote_ready.load(std::memory_order_relaxed) &&
            dynamic_ready.load(std::memory_order_relaxed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!local_ready.load(std::memory_order_relaxed) ||
        !remote_ready.load(std::memory_order_relaxed) ||
        !dynamic_ready.load(std::memory_order_relaxed)) {
        std::cerr << "target echo servers did not become ready" << std::endl;
        server->stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (local_echo.joinable()) local_echo.join();
        if (remote_echo.joinable()) remote_echo.join();
        if (dynamic_echo.joinable()) dynamic_echo.join();
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::string cli_cmd = shell_quote(cli_bin.string()) +
                          " --host 127.0.0.1 --port " + std::to_string(server_port) +
                          " --user cli --password cli-pass" +
                          " --strict-host-key-checking accept-new" +
                          " --known-hosts " + shell_quote(known_hosts.string()) +
                          " -L 127.0.0.1:" + std::to_string(local_port) + ":127.0.0.1:" + std::to_string(target_local_port) +
                          " -D 127.0.0.1:" + std::to_string(dynamic_port) +
                          " -R 127.0.0.1:" + std::to_string(remote_bind_port) + ":127.0.0.1:" + std::to_string(target_remote_port) +
                          " --command " + shell_quote("sleep 12") +
                          " > " + shell_quote(cli_out.string()) +
                          " 2> " + shell_quote(cli_err.string());
#ifndef _WIN32
    cli_cmd = "timeout 25s " + cli_cmd;
#endif

    std::atomic<int> cli_status{ -1 };
    std::atomic<bool> cli_done{ false };
    std::thread cli_thread([&]() {
        cli_status.store(run_command(cli_cmd), std::memory_order_relaxed);
        cli_done.store(true, std::memory_order_relaxed);
    });

    bool local_path_ok = false;
    bool remote_path_ok = false;
    bool dynamic_path_ok = false;

    for (int i = 0; i < 200 && !cli_done.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!local_path_ok) {
            SocketFd probe = connect_once("127.0.0.1", static_cast<uint16_t>(local_port));
            if (probe != kInvalidSocketFd) {
                set_socket_recv_timeout(probe, 1000);
                const std::string line = "mix-l\n";
                if (send(probe, line.data(), static_cast<int>(line.size()), 0) > 0) {
                    std::array<char, 256> buf{};
                    const int n = recv(probe, buf.data(), static_cast<int>(buf.size()), 0);
                    if (n > 0 && std::string(buf.data(), buf.data() + n) == "mix-l-reply\n") {
                        local_path_ok = true;
                    }
                }
                close_socket(probe);
            }
        }

        if (!remote_path_ok) {
            SocketFd probe = connect_once("127.0.0.1", static_cast<uint16_t>(remote_bind_port));
            if (probe != kInvalidSocketFd) {
                set_socket_recv_timeout(probe, 1000);
                const std::string line = "mix-r\n";
                if (send(probe, line.data(), static_cast<int>(line.size()), 0) > 0) {
                    std::array<char, 256> buf{};
                    const int n = recv(probe, buf.data(), static_cast<int>(buf.size()), 0);
                    if (n > 0 && std::string(buf.data(), buf.data() + n) == "mix-r-reply\n") {
                        remote_path_ok = true;
                    }
                }
                close_socket(probe);
            }
        }

        if (!dynamic_path_ok) {
            SocketFd probe = connect_once("127.0.0.1", static_cast<uint16_t>(dynamic_port));
            if (probe != kInvalidSocketFd) {
                set_socket_recv_timeout(probe, 1000);

                const std::array<uint8_t, 3> greeting = { 0x05, 0x01, 0x00 };
                (void)send(probe, reinterpret_cast<const char *>(greeting.data()), static_cast<int>(greeting.size()), 0);

                std::array<uint8_t, 2> method_reply{};
                const int method_n = recv(probe, reinterpret_cast<char *>(method_reply.data()), static_cast<int>(method_reply.size()), 0);
                if (method_n == 2 && method_reply[0] == 0x05 && method_reply[1] == 0x00) {
                    std::array<uint8_t, 10> connect_req = {
                        0x05,
                        0x01,
                        0x00,
                        0x01,
                        127,
                        0,
                        0,
                        1,
                        static_cast<uint8_t>((target_dynamic_port >> 8) & 0xFF),
                        static_cast<uint8_t>(target_dynamic_port & 0xFF)
                    };
                    (void)send(probe, reinterpret_cast<const char *>(connect_req.data()), static_cast<int>(connect_req.size()), 0);

                    std::array<uint8_t, 10> connect_reply{};
                    const int connect_n = recv(probe, reinterpret_cast<char *>(connect_reply.data()), static_cast<int>(connect_reply.size()), 0);
                    if (connect_n == 10 && connect_reply[0] == 0x05 && connect_reply[1] == 0x00) {
                        const std::string line = "mix-d\n";
                        if (send(probe, line.data(), static_cast<int>(line.size()), 0) > 0) {
                            std::array<char, 256> data_buf{};
                            const int n = recv(probe, data_buf.data(), static_cast<int>(data_buf.size()), 0);
                            if (n > 0 && std::string(data_buf.data(), data_buf.data() + n) == "mix-d-reply\n") {
                                dynamic_path_ok = true;
                            }
                        }
                    }
                }

                close_socket(probe);
            }
        }

        if (local_path_ok && remote_path_ok && dynamic_path_ok) {
            break;
        }
    }

    if (cli_thread.joinable()) {
        cli_thread.join();
    }

    if (!local_ok.load(std::memory_order_relaxed)) {
        SocketFd wake = connect_once("127.0.0.1", static_cast<uint16_t>(target_local_port));
        if (wake != kInvalidSocketFd) close_socket(wake);
    }
    if (!remote_ok.load(std::memory_order_relaxed)) {
        SocketFd wake = connect_once("127.0.0.1", static_cast<uint16_t>(target_remote_port));
        if (wake != kInvalidSocketFd) close_socket(wake);
    }
    if (!dynamic_ok.load(std::memory_order_relaxed)) {
        SocketFd wake = connect_once("127.0.0.1", static_cast<uint16_t>(target_dynamic_port));
        if (wake != kInvalidSocketFd) close_socket(wake);
    }

    server->stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    if (local_echo.joinable()) local_echo.join();
    if (remote_echo.joinable()) remote_echo.join();
    if (dynamic_echo.joinable()) dynamic_echo.join();

    int status = cli_status.load(std::memory_order_relaxed);
#ifndef _WIN32
    if (status == 256) {
        status = 0;
    }
#endif

    if (status != 0) {
        std::cerr << "release_ssh_cli mixed-forward command failed with status " << cli_status.load() << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    if (!local_path_ok || !remote_path_ok || !dynamic_path_ok) {
        std::cerr << "release_ssh_cli mixed-forward soak data path failed"
                  << " (L=" << local_path_ok
                  << ", R=" << remote_path_ok
                  << ", D=" << dynamic_path_ok << ")" << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    std::cout << "release_ssh_cli mixed-forward soak passed." << std::endl;
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
