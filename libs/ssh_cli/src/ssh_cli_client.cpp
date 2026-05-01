#include "ssh_cli_client.h"

#include "ssh_cli_shell_session.h"
#include "ssh_cli_transport.h"
#include "ssh_cli_transport_stub.h"

#include <memory>

namespace yuan::libs::ssh_cli
{
    SshCliClient::SshCliClient()
        : session_(std::make_unique<SshCliTransportStub>())
    {
    }

    SshCliClient::SshCliClient(std::unique_ptr<SshCliTransport> transport)
        : session_(std::move(transport))
    {
    }

    bool SshCliClient::connect(const SshCliConnectionOptions & options)
    {
        options_ = options;
        return session_.connect(options_);
    }

    bool SshCliClient::authenticate_password(const std::string & password)
    {
        return session_.authenticate_password(password);
    }

    bool SshCliClient::authenticate_publickey(const std::string & private_key_path)
    {
        return session_.authenticate_publickey(private_key_path);
    }

    bool SshCliClient::open_shell()
    {
        return session_.open_shell();
    }

    bool SshCliClient::run_command(const std::string & command,
                                   std::string * stdout_data)
    {
        return session_.run_command(command, stdout_data);
    }

    bool SshCliClient::read_stdout_chunk(std::string * chunk)
    {
        return session_.read_stdout_chunk(chunk);
    }

    bool SshCliClient::send_signal(const std::string & signal_name)
    {
        return session_.send_signal(signal_name);
    }

    bool SshCliClient::send_stdin(const std::string & chunk)
    {
        return session_.send_stdin(chunk);
    }

    bool SshCliClient::is_shell_alive() const
    {
        return session_.is_shell_alive();
    }

    std::unique_ptr<SshCliShellSession> SshCliClient::create_shell_session()
    {
        if (!is_shell_alive()) {
            return nullptr;
        }
        return std::make_unique<SshCliShellSession>(this);
    }

    void SshCliClient::close()
    {
        session_.close();
    }

    SshCliSessionState SshCliClient::state() const
    {
        const auto phase = session_.phase();
        SshCliSessionState s;
        s.connected = phase == SshCliPhase::connected ||
                      phase == SshCliPhase::authenticated ||
                      phase == SshCliPhase::shell_open;
        s.authenticated = phase == SshCliPhase::authenticated ||
                          phase == SshCliPhase::shell_open;
        s.shell_open = phase == SshCliPhase::shell_open;
        return s;
    }

    std::string SshCliClient::last_error() const
    {
        return session_.last_error();
    }
}
