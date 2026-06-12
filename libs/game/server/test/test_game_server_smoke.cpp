#include "common/rpc_network.h"
#include "option.h"
#include "redis_client.h"

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
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

    void stop_process(pid_t pid)
    {
        if (pid > 0) {
            (void)::kill(pid, SIGTERM);
        }
    }
#endif

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
#ifdef _WIN32
    (void)argc;
    (void)argv;
    std::cerr << "process smoke uses POSIX process spawning and is disabled on Windows\n";
    return EXIT_SUCCESS;
#else
    if (argc != 8) {
        std::cerr << "usage: test_game_server_smoke <global-bin> <tunnel-bin> <zone-bin> <world-bin> <gateway-bin> <web-bin> <mock-client-bin>\n";
        return 2;
    }

    const auto global_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto zone_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto tunnel_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto world_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto gateway_port = yuan::game::server::rpc_network::reserve_loopback_port();
    const auto web_port = yuan::game::server::rpc_network::reserve_loopback_port();
    if (!require(global_port != 0 && zone_port != 0 && tunnel_port != 0 && world_port != 0 && gateway_port != 0 && web_port != 0,
                 "ports should be reserved")) {
        return 3;
    }

    yuan::redis::Option redis_option;
    redis_option.host_ = "127.0.0.1";
    redis_option.port_ = 6379;
    redis_option.db_ = 0;
    redis_option.timeout_ms_ = 500;
    redis_option.connect_timeout_ms_ = 500;
    redis_option.name_ = "game-server-smoke-check";
    yuan::redis::RedisClient redis(redis_option);
    if (!redis.ensure_connected() || !redis.ping()) {
        std::cerr << "game_server_smoke skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return EXIT_SUCCESS;
    }

    const std::string template_dir = GAME_SERVER_CONFIG_DIR;
    const std::string config_dir = "/tmp/opencode";
    const auto suffix = std::to_string(::getpid());
    const auto tunnel_config = config_dir + "/game_tunnel_" + suffix + ".conf";
    const auto global_config = config_dir + "/game_global_" + suffix + ".conf";
    const auto zone_config = config_dir + "/game_zone_" + suffix + ".conf";
    const auto world_config = config_dir + "/game_world_" + suffix + ".conf";
    const auto gateway_config = config_dir + "/game_gateway_" + suffix + ".conf";
    const auto web_config = config_dir + "/game_web_" + suffix + ".conf";
    auto tunnel_template = read_file(template_dir + "/tunnel.conf");
    auto global_template = read_file(template_dir + "/global.conf");
    auto zone_template = read_file(template_dir + "/zone.conf");
    auto world_template = read_file(template_dir + "/world.conf");
    auto gateway_template = read_file(template_dir + "/gateway.conf");
    auto web_template = read_file(template_dir + "/web.conf");
    if (!require(!tunnel_template.empty() && !global_template.empty() && !zone_template.empty() && !world_template.empty() && !gateway_template.empty() && !web_template.empty(), "config templates should load")) {
        return 15;
    }
    replace_all(tunnel_template, "listen_port=0", "listen_port=" + std::to_string(tunnel_port));
    replace_all(tunnel_template, "expected_requests=4", "expected_requests=11");
    replace_all(global_template, "listen_port=0", "listen_port=" + std::to_string(global_port));
    replace_all(global_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(zone_template, "listen_port=0", "listen_port=" + std::to_string(zone_port));
    replace_all(zone_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(zone_template, "target_global_instance=1", "target_global_instance=0");
    replace_all(zone_template, "expected_requests=1", "expected_requests=3");
    replace_all(world_template, "listen_port=0", "listen_port=" + std::to_string(world_port));
    replace_all(world_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(world_template, "expected_requests=1", "expected_requests=6");
    replace_all(gateway_template, "listen_port=0", "listen_port=" + std::to_string(gateway_port));
    replace_all(gateway_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(gateway_template, "expected_requests=1", "expected_requests=4");
    replace_all(web_template, "listen_port=0", "listen_port=" + std::to_string(web_port));
    replace_all(web_template, "tunnel_port=0", "tunnel_port=" + std::to_string(tunnel_port));
    replace_all(web_template, "target_world_port=0", "target_world_port=" + std::to_string(world_port));
    replace_all(web_template, "target_world_ports=0", "target_world_ports=" + std::to_string(world_port));

    if (!require(write_file(tunnel_config, tunnel_template), "tunnel config should write")) {
        return 12;
    }

    if (!require(write_file(global_config, global_template), "global config should write")) {
        return 13;
    }
    
    if (!require(write_file(zone_config, zone_template), "zone config should write")) {
        return 14;
    }

    if (!require(write_file(world_config, world_template), "world config should write") ||
        !require(write_file(gateway_config, gateway_template), "gateway config should write") ||
        !require(write_file(web_config, web_template), "web config should write")) {
        return 16;
    }

    const pid_t tunnel_pid = spawn_process(argv[2], tunnel_config);
    if (!require(tunnel_pid > 0, "tunnel process should spawn")) {
        return 6;
    }
    wait_startup();

    const pid_t world_pid = spawn_process(argv[4], world_config);
    if (!require(world_pid > 0, "world process should spawn")) {
        return 17;
    }
    wait_startup();

    const pid_t zone_pid = spawn_process(argv[3], zone_config);
    if (!require(zone_pid > 0, "zone process should spawn")) {
        return 8;
    }
    wait_startup();

    const pid_t gateway_pid = spawn_process(argv[5], gateway_config);
    if (!require(gateway_pid > 0, "gateway process should spawn")) {
        return 18;
    }
    wait_startup();

    const pid_t web_pid = spawn_process(argv[6], web_config);
    if (!require(web_pid > 0, "web process should spawn")) {
        return 19;
    }
    wait_startup();

    const auto player_uid_text = std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    const pid_t client_pid = ::fork();
    if (client_pid == 0) {
        const auto web_port_text = std::to_string(web_port);
        ::execl(argv[7], argv[7], web_port_text.c_str(), player_uid_text.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    if (!require(client_pid > 0, "mock client should spawn")) {
        return 20;
    }

    const bool client_ok = wait_exit(client_pid);
    if (!client_ok) {
        stop_process(web_pid);
        stop_process(gateway_pid);
        stop_process(zone_pid);
        stop_process(world_pid);
        stop_process(tunnel_pid);
    }
    const bool web_ok = wait_exit(web_pid);
    const bool gateway_ok = wait_exit(gateway_pid);
    const bool zone_ok = wait_exit(zone_pid);
    const bool world_ok = wait_exit(world_pid);
    const bool tunnel_ok = wait_exit(tunnel_pid);
    if (!require(client_ok, "mock client should exit successfully")) {
        return 21;
    }
    if (!require(web_ok, "web process should exit successfully")) {
        return 22;
    }
    if (!require(gateway_ok, "gateway process should exit successfully")) {
        return 23;
    }
    if (!require(zone_ok, "zone process should exit successfully")) {
        return 9;
    }
    if (!require(world_ok, "world process should exit successfully")) {
        return 24;
    }
    if (!require(tunnel_ok, "tunnel process should exit successfully")) {
        return 10;
    }
    return EXIT_SUCCESS;
#endif
}
