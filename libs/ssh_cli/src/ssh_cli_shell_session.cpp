#include "ssh_cli_shell_session.h"

#include "ssh_cli_client.h"

#include <chrono>
#include <thread>

namespace yuan::libs::ssh_cli
{
    SshCliShellSession::SshCliShellSession(SshCliClient * client)
        : client_(client)
    {
    }

    bool SshCliShellSession::write(const std::string & chunk)
    {
        if (!open_ || !client_) {
            return false;
        }
        return client_->send_stdin(chunk);
    }

    bool SshCliShellSession::read(std::string * chunk)
    {
        if (!open_ || !client_) {
            return false;
        }
        return client_->read_stdout_chunk(chunk);
    }

    bool SshCliShellSession::read_until(const std::string & needle,
                                        std::string * captured,
                                        uint32_t timeout_ms,
                                        uint32_t poll_interval_ms,
                                        size_t max_capture_bytes)
    {
        if (!open_ || !client_ || needle.empty()) {
            return false;
        }

        std::string out;
        uint32_t waited = 0;
        const auto poll = poll_interval_ms == 0 ? 10U : poll_interval_ms;

        while (waited <= timeout_ms) {
            std::string chunk;
            if (client_->read_stdout_chunk(&chunk) && !chunk.empty()) {
                out += chunk;
                if (out.size() > max_capture_bytes) {
                    out.erase(0, out.size() - max_capture_bytes);
                }
                if (out.find(needle) != std::string::npos) {
                    if (captured) {
                        *captured = out;
                    }
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(poll));
            waited += poll;
        }

        if (captured) {
            *captured = out;
        }
        return false;
    }

    bool SshCliShellSession::send_signal(const std::string & signal_name)
    {
        if (!open_ || !client_) {
            return false;
        }
        return client_->send_signal(signal_name);
    }

    bool SshCliShellSession::is_alive() const
    {
        if (!open_ || !client_) {
            return false;
        }
        return client_->is_shell_alive();
    }

    void SshCliShellSession::close(bool graceful_remote_exit)
    {
        if (open_ && client_ && graceful_remote_exit) {
            (void)client_->send_stdin("exit\n");
        }
        open_ = false;
    }
}
