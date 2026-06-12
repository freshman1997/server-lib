#include "ssh_server_handler.h"

#include "net/acceptor/acceptor_factory.h"
#include "net/socket/socket.h"
#include "ssh_server.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace yuan::release_ssh
{
    namespace
    {
        struct CommandCaptureResult
        {
            std::vector<uint8_t> output;
            uint32_t exit_status = 127;
        };

        CommandCaptureResult run_command_capture(const std::string &command)
        {
            CommandCaptureResult result;
            if (command.empty()) {
                result.exit_status = 0;
                return result;
            }

#ifdef _WIN32
            FILE *pipe = _popen((command + " 2>&1").c_str(), "rb");
#else
            FILE *pipe = popen((command + " 2>&1").c_str(), "r");
#endif
            if (!pipe) {
                const char *message = "failed to start command\n";
                result.output.assign(message, message + std::strlen(message));
                return result;
            }

            std::array<char, 4096> buffer{};
            while (true) {
                const size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
                if (n > 0) {
                    result.output.insert(result.output.end(), buffer.begin(), buffer.begin() + n);
                }
                if (n < buffer.size()) {
                    if (std::feof(pipe) != 0 || std::ferror(pipe) != 0) {
                        break;
                    }
                }
            }

#ifdef _WIN32
            const int status = _pclose(pipe);
            result.exit_status = status >= 0 ? static_cast<uint32_t>(status) : 127;
#else
            const int status = pclose(pipe);
            if (status >= 0 && WIFEXITED(status)) {
                result.exit_status = static_cast<uint32_t>(WEXITSTATUS(status));
            }
#endif
            return result;
        }
    }

    StaticCredentialSshHandler::StaticCredentialSshHandler(std::string username,
                                                           std::string password,
                                                           bool enable_password_auth,
                                                           bool enable_port_forwarding)
        : username_(std::move(username)),
          password_(std::move(password)),
          enable_password_auth_(enable_password_auth),
          enable_port_forwarding_(enable_port_forwarding)
    {
    }

    yuan::net::ssh::SshAuthResult StaticCredentialSshHandler::on_authenticate(
        yuan::net::ssh::SshSession *,
        const std::string &username,
        const std::string &method,
        const yuan::net::ssh::SshAuthCredentials &credentials)
    {
        if (method != "password" || !enable_password_auth_ || username != username_) {
            return yuan::net::ssh::SshAuthResult::FAILURE;
        }
        return credentials.password == password_
                   ? yuan::net::ssh::SshAuthResult::SUCCESS
                   : yuan::net::ssh::SshAuthResult::FAILURE;
    }

    bool StaticCredentialSshHandler::on_channel_open(yuan::net::ssh::SshSession *,
                                                     const std::string &channel_type,
                                                     yuan::net::ssh::SshChannel *)
    {
        if (channel_type == yuan::net::ssh::SSH_CHANNEL_SESSION) {
            return true;
        }
        if (channel_type == yuan::net::ssh::SSH_CHANNEL_DIRECT_TCPIP) {
            return enable_port_forwarding_;
        }
        return false;
    }

    bool StaticCredentialSshHandler::on_direct_tcpip(yuan::net::ssh::SshSession *,
                                                     yuan::net::ssh::SshChannel *,
                                                     const std::string &,
                                                     uint16_t)
    {
        return enable_port_forwarding_;
    }

    bool StaticCredentialSshHandler::on_pty_request(yuan::net::ssh::SshSession *,
                                                    yuan::net::ssh::SshChannel *,
                                                    const std::string &,
                                                    uint32_t,
                                                    uint32_t,
                                                    uint32_t,
                                                    uint32_t,
                                                    const std::vector<uint8_t> &)
    {
        return true;
    }

    bool StaticCredentialSshHandler::on_shell_request(yuan::net::ssh::SshSession *,
                                                      yuan::net::ssh::SshChannel *)
    {
        return true;
    }

    bool StaticCredentialSshHandler::on_exec_request(yuan::net::ssh::SshSession *session,
                                                     yuan::net::ssh::SshChannel *channel,
                                                     const std::string &command)
    {
        if (!session || !channel) {
            return false;
        }

        auto result = run_command_capture(command);
        if (!result.output.empty()) {
            session->enqueue_outgoing(
                session->connection_manager().build_channel_data(channel->remote_id(), result.output));
        }
        session->enqueue_outgoing(
            session->connection_manager().build_channel_exit_status(channel->remote_id(), result.exit_status));
        session->enqueue_outgoing(session->connection_manager().build_channel_eof(channel->remote_id()));
        session->enqueue_outgoing(session->connection_manager().build_channel_close(channel->remote_id()));
        return true;
    }

    bool StaticCredentialSshHandler::on_env_request(yuan::net::ssh::SshSession *,
                                                    yuan::net::ssh::SshChannel *,
                                                    const std::string &,
                                                    const std::string &)
    {
        return true;
    }

    bool StaticCredentialSshHandler::enable_builtin_exec_bridge() const
    {
        return false;
    }

    bool StaticCredentialSshHandler::on_global_request(yuan::net::ssh::SshSession *,
                                                       const std::string &request_name,
                                                       const std::vector<uint8_t> &)
    {
        return request_name == "keepalive@openssh.com" || request_name == "no-more-sessions@openssh.com";
    }

    uint16_t StaticCredentialSshHandler::on_tcpip_forward(yuan::net::ssh::SshSession *session,
                                                          const std::string &bind_addr,
                                                          uint16_t bind_port)
    {
        if (!enable_port_forwarding_ || !session || !session->server()) {
            return 0;
        }

        auto *runtime = session->server()->runtime();
        if (!runtime || !runtime->event_loop()) {
            return 0;
        }

        auto socket = std::make_unique<yuan::net::Socket>(bind_addr, bind_port);
        if (!socket->valid()) {
            return 0;
        }
#ifdef _WIN32
        socket->set_reuse(true, true);
#else
        socket->set_reuse(true);
#endif
        socket->set_none_block(true);
        if (!socket->bind()) {
            return 0;
        }

        const uint16_t allocated_port = static_cast<uint16_t>(socket->get_local_address().get_port());
        auto acceptor = std::shared_ptr<yuan::net::StreamAcceptor>(
            yuan::net::create_stream_acceptor(socket.release()),
            [](yuan::net::StreamAcceptor *ptr) {
                if (ptr) {
                    ptr->close();
                    delete ptr;
                }
            });
        if (!acceptor || !acceptor->listen()) {
            return 0;
        }

        acceptor->set_event_handler(runtime->event_loop());
        acceptor->update_channel();

        std::lock_guard<std::mutex> lock(remote_listener_mutex_);
        pending_remote_listeners_[forward_key(bind_addr, allocated_port)] = acceptor;
        return allocated_port;
    }

    void StaticCredentialSshHandler::on_cancel_tcpip_forward(yuan::net::ssh::SshSession *,
                                                             const std::string &bind_addr,
                                                             uint16_t bind_port)
    {
        std::shared_ptr<yuan::net::StreamAcceptor> acceptor;
        {
            std::lock_guard<std::mutex> lock(remote_listener_mutex_);
            auto it = pending_remote_listeners_.find(forward_key(bind_addr, bind_port));
            if (it != pending_remote_listeners_.end()) {
                acceptor = std::move(it->second);
                pending_remote_listeners_.erase(it);
            }
        }
        if (acceptor) {
            acceptor->close();
        }
    }

    std::shared_ptr<yuan::net::StreamAcceptor> StaticCredentialSshHandler::on_forwarded_tcpip_listener(
        yuan::net::ssh::SshSession *,
        const std::string &bind_addr,
        uint16_t bind_port)
    {
        std::lock_guard<std::mutex> lock(remote_listener_mutex_);
        auto it = pending_remote_listeners_.find(forward_key(bind_addr, bind_port));
        if (it == pending_remote_listeners_.end()) {
            return {};
        }
        auto acceptor = std::move(it->second);
        pending_remote_listeners_.erase(it);
        return acceptor;
    }

    std::string StaticCredentialSshHandler::forward_key(const std::string &bind_addr, uint16_t bind_port)
    {
        return bind_addr + ":" + std::to_string(bind_port);
    }
}
