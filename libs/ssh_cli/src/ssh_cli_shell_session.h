#ifndef __LIBS_SSH_CLI_SHELL_SESSION_H__
#define __LIBS_SSH_CLI_SHELL_SESSION_H__

#include <cstdint>
#include <string>

namespace yuan::libs::ssh_cli
{
    class SshCliClient;

    class SshCliShellSession
    {
    public:
        explicit SshCliShellSession(SshCliClient *client);

        bool write(const std::string &chunk);
        bool read(std::string *chunk);
        bool read_until(const std::string &needle,
                        std::string *captured,
                        uint32_t timeout_ms = 1200,
                        uint32_t poll_interval_ms = 10,
                        size_t max_capture_bytes = 256 * 1024);
        bool send_signal(const std::string &signal_name);
        bool is_alive() const;
        void close(bool graceful_remote_exit = true);

    private:
        SshCliClient *client_ = nullptr;
        bool open_ = true;
    };
}

#endif
