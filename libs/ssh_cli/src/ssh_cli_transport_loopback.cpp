#include "ssh_cli_transport_loopback.h"

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

    bool SshCliTransportLoopback::connect(const SshCliConnectionOptions & options,
                                          std::string * error)
    {
        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;
        captured_stdin_.clear();

        if (options.host.empty() || options.port == 0 || options.username.empty()) {
            set_error(error, "host/port/username are required");
            return false;
        }
        connected_ = true;
        return true;
    }

    bool SshCliTransportLoopback::authenticate_password(const std::string & password,
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

    bool SshCliTransportLoopback::authenticate_publickey(const std::string & private_key_path,
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

    bool SshCliTransportLoopback::open_shell(std::string * error)
    {
        if (!connected_ || !authenticated_) {
            set_error(error, "must connect and authenticate first");
            return false;
        }
        shell_open_ = true;
        return true;
    }

    bool SshCliTransportLoopback::run_command(const std::string & command,
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
            *stdout_data = "loopback:" + command;
        }
        return true;
    }

    bool SshCliTransportLoopback::read_stdout_chunk(std::string * chunk,
                                                    std::string * error)
    {
        (void)error;
        if (chunk) {
            *chunk = std::string();
        }
        return false;
    }

    bool SshCliTransportLoopback::send_signal(const std::string & signal_name,
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

    bool SshCliTransportLoopback::is_shell_alive() const
    {
        return shell_open_;
    }

    bool SshCliTransportLoopback::send_stdin(const std::string & chunk,
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
        captured_stdin_ += chunk;
        return true;
    }

    void SshCliTransportLoopback::close()
    {
        connected_ = false;
        authenticated_ = false;
        shell_open_ = false;
    }

    const std::string &SshCliTransportLoopback::captured_stdin() const
    {
        return captured_stdin_;
    }
}
