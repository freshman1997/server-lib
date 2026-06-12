#include "common/tcp_rpc.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    std::uint16_t reserve_port()
    {
        const int fd = yuan::game::server::tcp_rpc::listen_loopback(0);
        if (fd < 0) {
            return 0;
        }
        sockaddr_in address{};
        socklen_t len = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &len) != 0) {
            yuan::game::server::tcp_rpc::close_fd(fd);
            return 0;
        }
        const auto port = ntohs(address.sin_port);
        yuan::game::server::tcp_rpc::close_fd(fd);
        return port;
    }

    pid_t spawn_process(const std::string &path, const std::string &config_path)
    {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::execl(path.c_str(), path.c_str(), config_path.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        return pid;
    }

    bool wait_exit(pid_t pid)
    {
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) {
            return false;
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    void wait_startup()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool write_file(const std::string &path, const std::string &content)
    {
        std::ofstream output(path);
        if (!output) {
            return false;
        }
        output << content;
        return static_cast<bool>(output);
    }

    std::string read_file(const std::string &path)
    {
        std::ifstream input(path);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    void replace_all(std::string &text, const std::string &from, const std::string &to)
    {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: test_game_server_process_smoke <global-bin> <tunnel-bin> <zone-bin>\n";
        return 2;
    }

    const auto global_port = reserve_port();
    const auto zone_port = reserve_port();
    const auto tunnel_port = reserve_port();
    if (!require(global_port != 0 && zone_port != 0 && tunnel_port != 0 && global_port != tunnel_port && zone_port != tunnel_port && zone_port != global_port,
                 "ports should be reserved")) {
        return 3;
    }

    const std::string template_dir = GAME_SERVER_CONFIG_DIR;
    const std::string config_dir = "/tmp/opencode";
    const auto suffix = std::to_string(::getpid());
    const auto tunnel_config = config_dir + "/game_tunnel_" + suffix + ".conf";
    const auto global_config = config_dir + "/game_global_" + suffix + ".conf";
    const auto zone_config = config_dir + "/game_zone_" + suffix + ".conf";
    auto tunnel_template = read_file(template_dir + "/tunnel.conf");
    auto global_template = read_file(template_dir + "/global.conf");
    auto zone_template = read_file(template_dir + "/zone.conf");
    if (!require(!tunnel_template.empty() && !global_template.empty() && !zone_template.empty(), "config templates should load")) {
        return 15;
    }
    replace_all(tunnel_template, "listen_port=0", "listen_port=" + std::to_string(tunnel_port));
    replace_all(tunnel_template, "expected_requests=4", "expected_requests=4");
    replace_all(global_template, "listen_port=0", "listen_port=" + std::to_string(global_port));
    replace_all(global_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(zone_template, "listen_port=0", "listen_port=" + std::to_string(zone_port));
    replace_all(zone_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));

    if (!require(write_file(tunnel_config, tunnel_template), "tunnel config should write")) {
        return 12;
    }

    if (!require(write_file(global_config, global_template), "global config should write")) {
        return 13;
    }
    
    if (!require(write_file(zone_config, zone_template), "zone config should write")) {
        return 14;
    }

    const pid_t tunnel_pid = spawn_process(argv[2], tunnel_config);
    if (!require(tunnel_pid > 0, "tunnel process should spawn")) {
        return 6;
    }
    wait_startup();

    const pid_t global_pid = spawn_process(argv[1], global_config);
    if (!require(global_pid > 0, "global process should spawn")) {
        return 4;
    }
    wait_startup();

    const pid_t zone_pid = spawn_process(argv[3], zone_config);
    if (!require(zone_pid > 0, "zone process should spawn")) {
        return 8;
    }

    const bool zone_ok = wait_exit(zone_pid);
    const bool tunnel_ok = wait_exit(tunnel_pid);
    const bool global_ok = wait_exit(global_pid);
    if (!require(zone_ok, "zone process should exit successfully")) {
        return 9;
    }
    if (!require(tunnel_ok, "tunnel process should exit successfully")) {
        return 10;
    }
    if (!require(global_ok, "global process should exit successfully")) {
        return 11;
    }
    return EXIT_SUCCESS;
}
