#include "ssh_cli_session.h"

#include "ssh_cli_client.h"
#include "ssh_cli_transport.h"
#include "ssh_cli_transport_stub.h"

#include <memory>

namespace yuan::libs::ssh_cli
{
    SshCliSession::SshCliSession(std::unique_ptr<SshCliTransport> transport)
        : transport_(std::move(transport))
    {
        if (!transport_) {
            transport_ = std::make_unique<SshCliTransportStub>();
        }
    }

    SshCliSession::~SshCliSession() = default;

    bool SshCliSession::connect(const SshCliConnectionOptions & options)
    {
        phase_ = SshCliPhase::idle;
        last_error_.clear();

        if (!transport_->connect(options, &last_error_)) {
            if (last_error_.empty()) {
                set_error("connect failed");
            }
            return false;
        }

        phase_ = SshCliPhase::connected;
        return true;
    }

    bool SshCliSession::authenticate_password(const std::string & password)
    {
        if (!transport_->authenticate_password(password, &last_error_)) {
            if (last_error_.empty()) {
                set_error("password authentication failed");
            }
            return false;
        }
        phase_ = SshCliPhase::authenticated;
        return true;
    }

    bool SshCliSession::authenticate_publickey(const std::string & private_key_path)
    {
        if (!transport_->authenticate_publickey(private_key_path, &last_error_)) {
            if (last_error_.empty()) {
                set_error("publickey authentication failed");
            }
            return false;
        }
        phase_ = SshCliPhase::authenticated;
        return true;
    }

    bool SshCliSession::open_shell()
    {
        if (!transport_->open_shell(&last_error_)) {
            if (last_error_.empty()) {
                set_error("open shell failed");
            }
            return false;
        }
        phase_ = SshCliPhase::shell_open;
        return true;
    }

    bool SshCliSession::run_command(const std::string & command,
                                    std::string * stdout_data)
    {
        if (!transport_->run_command(command, stdout_data, &last_error_)) {
            if (last_error_.empty()) {
                set_error("run command failed");
            }
            return false;
        }
        return true;
    }

    bool SshCliSession::read_stdout_chunk(std::string * chunk)
    {
        if (!transport_->read_stdout_chunk(chunk, &last_error_)) {
            return false;
        }
        return true;
    }

    bool SshCliSession::send_signal(const std::string & signal_name)
    {
        if (!transport_->send_signal(signal_name, &last_error_)) {
            return false;
        }
        return true;
    }

    bool SshCliSession::send_stdin(const std::string & chunk)
    {
        if (!transport_->send_stdin(chunk, &last_error_)) {
            if (last_error_.empty()) {
                set_error("send stdin failed");
            }
            return false;
        }
        return true;
    }

    bool SshCliSession::is_shell_alive() const
    {
        return transport_ && transport_->is_shell_alive();
    }

    void SshCliSession::close()
    {
        transport_->close();
        phase_ = SshCliPhase::idle;
        last_error_.clear();
    }

    SshCliPhase SshCliSession::phase() const
    {
        return phase_;
    }

    const std::string &SshCliSession::last_error() const
    {
        return last_error_;
    }

    void SshCliSession::set_error(const std::string & message)
    {
        last_error_ = message;
    }
}
