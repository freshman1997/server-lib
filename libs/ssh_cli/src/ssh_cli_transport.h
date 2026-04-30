#ifndef __LIBS_SSH_CLI_TRANSPORT_H__
#define __LIBS_SSH_CLI_TRANSPORT_H__

#include <string>

namespace yuan::libs::ssh_cli
{
    struct SshCliConnectionOptions;

    class SshCliTransport
    {
    public:
        virtual ~SshCliTransport() = default;

        virtual bool connect(const SshCliConnectionOptions &options,
                             std::string *error) = 0;
        virtual bool authenticate_password(const std::string &password,
                                           std::string *error) = 0;
        virtual bool authenticate_publickey(const std::string &private_key_path,
                                            std::string *error) = 0;
        virtual bool open_shell(std::string *error) = 0;
        virtual bool run_command(const std::string &command,
                                 std::string *stdout_data,
                                 std::string *error) = 0;
        virtual bool read_stdout_chunk(std::string *chunk,
                                       std::string *error) = 0;
        virtual bool send_signal(const std::string &signal_name,
                                 std::string *error) = 0;
        virtual bool send_stdin(const std::string &chunk,
                                std::string *error) = 0;
        virtual bool is_shell_alive() const = 0;
        virtual void close() = 0;
    };
}

#endif
