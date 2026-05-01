#ifndef __LIBS_SSH_CLI_TRANSPORT_PROCESS_H__
#define __LIBS_SSH_CLI_TRANSPORT_PROCESS_H__

#include "ssh_cli_client.h"
#include "ssh_cli_transport.h"

namespace yuan::libs::ssh_cli
{
    class SshCliTransportProcess final : public SshCliTransport
    {
    public:
        ~SshCliTransportProcess() override;

        bool connect(const SshCliConnectionOptions &options,
                     std::string *error) override;
        bool authenticate_password(const std::string &password,
                                   std::string *error) override;
        bool authenticate_publickey(const std::string &private_key_path,
                                    std::string *error) override;
        bool open_shell(std::string *error) override;
        bool run_command(const std::string &command,
                         std::string *stdout_data,
                         std::string *error) override;
        bool read_stdout_chunk(std::string *chunk,
                               std::string *error) override;
        bool send_signal(const std::string &signal_name,
                         std::string *error) override;
        bool send_stdin(const std::string &chunk,
                        std::string *error) override;
        bool is_shell_alive() const override;
        void close() override;

    private:
        bool connected_ = false;
        bool authenticated_ = false;
        bool shell_open_ = false;
        SshCliConnectionOptions options_;
        int shell_child_pid_ = -1;
        int shell_stdin_fd_ = -1;
        int shell_stdout_fd_ = -1;
    };
}

#endif
