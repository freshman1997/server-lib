#ifndef YUAN_RELEASE_SSH_SERVER_HANDLER_H
#define YUAN_RELEASE_SSH_SERVER_HANDLER_H

#include "ssh_handler.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace yuan::net
{
    class StreamAcceptor;
}

namespace yuan::release_ssh
{
    class StaticCredentialSshHandler final : public yuan::net::ssh::SshHandler
    {
    public:
        StaticCredentialSshHandler(std::string username,
                                   std::string password,
                                   bool enable_password_auth,
                                   bool enable_port_forwarding);

        yuan::net::ssh::SshAuthResult on_authenticate(
            yuan::net::ssh::SshSession *session,
            const std::string &username,
            const std::string &method,
            const yuan::net::ssh::SshAuthCredentials &credentials) override;

        bool on_channel_open(yuan::net::ssh::SshSession *session,
                             const std::string &channel_type,
                             yuan::net::ssh::SshChannel *channel) override;

        bool on_direct_tcpip(yuan::net::ssh::SshSession *session,
                             yuan::net::ssh::SshChannel *channel,
                             const std::string &target_host,
                             uint16_t target_port) override;

        bool on_pty_request(yuan::net::ssh::SshSession *session,
                            yuan::net::ssh::SshChannel *channel,
                            const std::string &term,
                            uint32_t width,
                            uint32_t height,
                            uint32_t pixel_width,
                            uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override;

        bool on_shell_request(yuan::net::ssh::SshSession *session,
                              yuan::net::ssh::SshChannel *channel) override;

        bool on_exec_request(yuan::net::ssh::SshSession *session,
                             yuan::net::ssh::SshChannel *channel,
                             const std::string &command) override;

        bool on_env_request(yuan::net::ssh::SshSession *session,
                            yuan::net::ssh::SshChannel *channel,
                            const std::string &name,
                            const std::string &value) override;

        bool enable_builtin_exec_bridge() const override;

        bool on_global_request(yuan::net::ssh::SshSession *session,
                               const std::string &request_name,
                               const std::vector<uint8_t> &data) override;

        uint16_t on_tcpip_forward(yuan::net::ssh::SshSession *session,
                                  const std::string &bind_addr,
                                  uint16_t bind_port) override;

        void on_cancel_tcpip_forward(yuan::net::ssh::SshSession *session,
                                     const std::string &bind_addr,
                                     uint16_t bind_port) override;

        std::shared_ptr<yuan::net::StreamAcceptor> on_forwarded_tcpip_listener(
            yuan::net::ssh::SshSession *session,
            const std::string &bind_addr,
            uint16_t bind_port) override;

    private:
        static std::string forward_key(const std::string &bind_addr, uint16_t bind_port);

        std::string username_;
        std::string password_;
        bool enable_password_auth_ = true;
        bool enable_port_forwarding_ = true;
        std::mutex remote_listener_mutex_;
        std::unordered_map<std::string, std::shared_ptr<yuan::net::StreamAcceptor>> pending_remote_listeners_;
    };
}

#endif
