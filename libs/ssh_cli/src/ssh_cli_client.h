#ifndef __LIBS_SSH_CLI_CLIENT_H__
#define __LIBS_SSH_CLI_CLIENT_H__

#include "ssh_cli_session.h"
#include "ssh_cli_shell_session.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yuan::libs::ssh_cli
{
    struct SshCliConnectionOptions
    {
        std::string host = "127.0.0.1";
        uint16_t port = 22;
        std::string username;
        std::string password;
        std::string private_key_path;
        bool strict_host_key_checking = false;
        bool batch_mode = true;
        std::string known_hosts_path;
        std::vector<std::string> extra_ssh_options;
        int interactive_read_timeout_ms = 80;
        int interactive_poll_interval_ms = 10;
    };

    struct SshCliSessionState
    {
        bool connected = false;
        bool authenticated = false;
        bool shell_open = false;
    };

    class SshCliClient
    {
    public:
        SshCliClient();
        explicit SshCliClient(std::unique_ptr<SshCliTransport> transport);
        ~SshCliClient() = default;

        bool connect(const SshCliConnectionOptions &options);
        bool authenticate_password(const std::string &password);
        bool authenticate_publickey(const std::string &private_key_path);
        bool open_shell();
        bool run_command(const std::string &command, std::string *stdout_data);
        bool read_stdout_chunk(std::string *chunk);
        bool send_signal(const std::string &signal_name);
        bool send_stdin(const std::string &chunk);
        bool is_shell_alive() const;
        std::unique_ptr<SshCliShellSession> create_shell_session();
        void close();

        SshCliSessionState state() const;
        std::string last_error() const;

    private:
        SshCliConnectionOptions options_;
        SshCliSession session_;
    };
}

#endif
