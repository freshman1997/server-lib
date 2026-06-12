#include "repl.h"
#include "formatter.h"
#include "value/error_value.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "logger.h"
#include "platform/native_platform.h"
#include "option.h"

static int safe_stoi(const std::string &s, int default_val = 0)
{
    try
    {
        return std::stoi(s);
    }
    catch (const std::exception &)
    {
        std::cerr << "Invalid number: " << s << "\n";
        return default_val;
    }
}

static void print_usage()
{
    std::cout << "yredis - Redis CLI Tool (yuan::redis)\n\n"
              << "Usage:\n"
              << "  yredis [options]\n"
              << "  yredis [options] -- <command> [arg ...]\n"
              << "  yredis [options] <command> [arg ...]\n\n"
              << "Options:\n"
              << "  -h <host>       Redis host (default: localhost)\n"
              << "  -p <port>       Redis port (default: 6379)\n"
              << "  -d <db>         Database number (default: 0)\n"
              << "  -a <password>   Authentication password\n"
              << "  -u <username>   Authentication username\n"
              << "  -t <ms>         Command timeout in ms\n"
              << "  -n <name>       Client name\n"
              << "  -c              Connect on startup\n"
              << "  --raw            Raw output format\n"
              << "  --help           Show this help\n\n"
              << "Environment:\n"
              << "  YREDIS_AUTH     Password (overrides -a)\n\n"
              << "Examples:\n"
              << "  yredis -c                    Connect to localhost:6379 immediately\n"
              << "  yredis -h 127.0.0.1 -p 6380  Set host and port\n"
              << "  yredis -c -a mypassword       Connect with authentication\n"
              << "  yredis -c -d 2                 Connect to database 2\n"
              << "  yredis -c GET mykey            Run one command and exit\n"
              << "  yredis -c -- SET mykey value   Use -- before commands starting with '-'\n\n";
}

static bool stdin_is_tty()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

static int run_one_command(yredis::ReplState &state, const std::vector<std::string> &command)
{
    if (command.empty()) {
        return 0;
    }

    state.client = std::make_shared<yuan::redis::RedisClient>(state.opt);
    if (!state.client->ensure_connected()) {
        std::cerr << yredis::ansi_red() << "Failed to connect to "
                  << state.opt.host_ << ":" << state.opt.port_ << yredis::ansi_reset() << "\n";
        state.client->close();
        state.client.reset();
        return 1;
    }

    std::vector<std::string> args(command.begin() + 1, command.end());
    auto result = state.client->command(command[0], args);
    std::cout << yredis::format_value(result, state.format_style);

    const auto err = result ? result->as<yuan::redis::ErrorValue>() : nullptr;
    state.client->close();
    state.client.reset();
    return err ? 2 : 0;
}

int main(int argc, char *argv[])
{
    yuan::platform::NativePlatformGuard guard;
    LOG_GET_REGISTRY()->set_global_level(yuan::log::Level::warn);
    LOG_GET_REGISTRY()->disable_file_log();

    yredis::ReplState state;
    bool auto_connect = false;
    std::vector<std::string> one_shot_command;

    const char *env_auth = std::getenv("YREDIS_AUTH");
    if (env_auth && env_auth[0] != '\0') {
        state.opt.password_ = env_auth;
    }

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--")
        {
            for (++i; i < argc; ++i) {
                one_shot_command.emplace_back(argv[i]);
            }
            break;
        }
        else if (arg == "--help")
        {
            print_usage();
            return 0;
        }
        else if (arg == "--raw")
        {
            state.format_style = yredis::FormatStyle::raw;
        }
        else if (arg == "-c")
        {
            auto_connect = true;
        }
        else if (arg == "-h" && i + 1 < argc)
        {
            state.opt.host_ = argv[++i];
        }
        else if (arg == "-p" && i + 1 < argc)
        {
            state.opt.port_ = safe_stoi(argv[++i], state.opt.port_);
        }
        else if (arg == "-d" && i + 1 < argc)
        {
            state.opt.db_ = safe_stoi(argv[++i], state.opt.db_);
        }
        else if (arg == "-a" && i + 1 < argc)
        {
            state.opt.password_ = argv[++i];
            if (argv[i]) {
                std::memset(argv[i], 'x', std::strlen(argv[i]));
            }
        }
        else if (arg == "-u" && i + 1 < argc)
        {
            state.opt.username_ = argv[++i];
        }
        else if (arg == "-t" && i + 1 < argc)
        {
            state.opt.command_timeout_ms_ = safe_stoi(argv[++i], state.opt.command_timeout_ms_);
        }
        else if (arg == "-n" && i + 1 < argc)
        {
            state.opt.name_ = argv[++i];
        }
        else
        {
            one_shot_command.emplace_back(std::move(arg));
            for (++i; i < argc; ++i) {
                one_shot_command.emplace_back(argv[i]);
            }
            break;
        }
    }

    if (!one_shot_command.empty())
    {
        return run_one_command(state, one_shot_command);
    }

    if (!stdin_is_tty())
    {
        state.quiet = true;
        yredis::repl_loop(state);
        return 0;
    }

    yredis::print_banner();

    if (auto_connect)
    {
        state.client = std::make_shared<yuan::redis::RedisClient>(state.opt);
        bool ok = state.client->ensure_connected();
        if (!ok)
        {
            std::cerr << yredis::ansi_red() << "Failed to connect to "
                      << state.opt.host_ << ":" << state.opt.port_ << yredis::ansi_reset() << "\n";
            state.client->close();
            state.client.reset();
        }
        else
        {
            state.connected = true;
            std::cout << yredis::ansi_green() << "Connected to "
                      << state.opt.host_ << ":" << state.opt.port_ << " db=" << state.opt.db_
                      << yredis::ansi_reset() << "\n";

            if (state.opt.db_ > 0)
            {
                auto result = state.client->select(state.opt.db_);
                if (result && !result->as<yuan::redis::ErrorValue>())
                {
                    std::cout << yredis::ansi_green() << "Database " << state.opt.db_ << " selected." << yredis::ansi_reset() << "\n";
                }
                else
                {
                    std::cout << yredis::ansi_red() << "Failed to select db " << state.opt.db_ << yredis::ansi_reset() << "\n";
                }
            }
        }
    }

    yredis::repl_loop(state);
    return 0;
}
