#ifndef __LIBS_SSH_CLI_TEST_COMMON_H__
#define __LIBS_SSH_CLI_TEST_COMMON_H__

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace yuan::libs::ssh_cli::test_common
{
    inline bool enabled_by_env(const char *name)
    {
        const char *v = std::getenv(name);
        return v && std::string(v) == "1";
    }

    inline bool recreate_dir(const std::filesystem::path &dir,
                             std::string *error)
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        return true;
    }

    inline std::string shell_quote(const std::string &value)
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

    inline bool run_command(const std::string &cmd)
    {
        return std::system(cmd.c_str()) == 0;
    }

    inline std::string build_ed25519_keygen_command(const std::filesystem::path &key_path)
    {
#ifdef _WIN32
        return "cmd /c ssh-keygen -q -t ed25519 -N \"\" -f " + shell_quote(key_path.string()) + " >nul 2>nul";
#else
        return "ssh-keygen -q -t ed25519 -N '' -f " + shell_quote(key_path.string()) + " >/dev/null 2>&1";
#endif
    }

    inline bool generate_ed25519_keypair(const std::filesystem::path &key_path)
    {
        return run_command(build_ed25519_keygen_command(key_path));
    }

    inline int choose_bindable_port(int start_port,
                                    int max_attempts)
    {
#if defined(_WIN32)
        (void)start_port;
        (void)max_attempts;
        return 22440;
#else
        for (int port = start_port; port < start_port + max_attempts; ++port) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                continue;
            }
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(static_cast<uint16_t>(port));

            const bool ok = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
            ::close(fd);
            if (ok) {
                return port;
            }
        }
        return 0;
#endif
    }
}

#endif
