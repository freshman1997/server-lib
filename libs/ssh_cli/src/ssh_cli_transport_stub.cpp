#include "ssh_cli_transport_stub.h"

#include "ssh_cli_client.h"

namespace yuan::libs::ssh_cli
{
    namespace
    {
        void set_error(std::string *error,
                       const std::string &message)
        {
            if (error) {
                *error = message;
            }
        }
    }

    bool SshCliTransportStub::connect(const SshCliConnectionOptions & options,
                                      std::string * error)
    {
        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;

        if (options.host.empty() || options.port == 0 || options.username.empty()) {
            set_error(error, "host/port/username are required");
            return false;
        }
        connected_ = true;
        return true;
    }

    bool SshCliTransportStub::authenticate_password(const std::string & password,
                                                    std::string * error)
    {
        if (!connected_) {
            set_error(error, "not connected");
            return false;
        }
        if (password.empty()) {
            set_error(error, "password cannot be empty");
            return false;
        }
        authenticated_ = true;
        return true;
    }

    bool SshCliTransportStub::authenticate_publickey(const std::string & private_key_path,
                                                     std::string * error)
    {
        if (!connected_) {
            set_error(error, "not connected");
            return false;
        }
        if (private_key_path.empty()) {
            set_error(error, "private key path cannot be empty");
            return false;
        }
        authenticated_ = true;
        return true;
    }

    bool SshCliTransportStub::open_shell(std::string * error)
    {
        if (!connected_ || !authenticated_) {
            set_error(error, "must connect and authenticate first");
            return false;
        }
        shell_open_ = true;
        return true;
    }

    bool SshCliTransportStub::run_command(const std::string & command,
                                          std::string * stdout_data,
                                          std::string * error)
    {
        if (!connected_ || !authenticated_) {
            set_error(error, "must connect and authenticate first");
            return false;
        }
        if (command.empty()) {
            set_error(error, "command cannot be empty");
            return false;
        }
        if (stdout_data) {
            *stdout_data = "stub:" + command;
        }
        return true;
    }

    bool SshCliTransportStub::read_stdout_chunk(std::string * chunk,
                                                std::string * error)
    {
        (void)error;
        if (chunk) {
            chunk->clear();
        }
        return false;
    }

    bool SshCliTransportStub::send_signal(const std::string & signal_name,
                                          std::string * error)
    {
        if (!shell_open_) {
            set_error(error, "shell is not open");
            return false;
        }
        if (signal_name.empty()) {
            set_error(error, "signal name cannot be empty");
            return false;
        }
        return true;
    }

    bool SshCliTransportStub::is_shell_alive() const
    {
        return shell_open_;
    }

    bool SshCliTransportStub::send_stdin(const std::string & chunk,
                                         std::string * error)
    {
        if (!shell_open_) {
            set_error(error, "shell is not open");
            return false;
        }
        if (chunk.empty()) {
            set_error(error, "stdin chunk cannot be empty");
            return false;
        }
        return true;
    }

    void SshCliTransportStub::close()
    {
        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;
    }
}
