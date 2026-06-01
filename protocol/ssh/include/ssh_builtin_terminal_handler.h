#ifndef __NET_SSH_SSH_BUILTIN_TERMINAL_HANDLER_H__
#define __NET_SSH_SSH_BUILTIN_TERMINAL_HANDLER_H__

#include "ssh_handler.h"

namespace yuan::net::ssh
{
    class SshBuiltinTerminalHandler final : public SshHandler
    {
    public:
        bool on_channel_open(SshSession *session,
                             const std::string &channel_type,
                             SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return channel_type == SSH_CHANNEL_SESSION ||
                   channel_type == SSH_CHANNEL_DIRECT_TCPIP;
        }

        bool on_direct_tcpip(SshSession *session,
                             SshChannel *channel,
                             const std::string &target_host,
                             uint16_t target_port) override
        {
            (void)session;
            (void)channel;
            (void)target_host;
            (void)target_port;
            return true;
        }

        bool on_pty_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &term,
                            uint32_t width, uint32_t height,
                            uint32_t pixel_width, uint32_t pixel_height,
                            const std::vector<uint8_t> &modes) override
        {
            (void)session;
            (void)channel;
            (void)term;
            (void)width;
            (void)height;
            (void)pixel_width;
            (void)pixel_height;
            (void)modes;
            return true;
        }

        bool on_shell_request(SshSession *session,
                              SshChannel *channel) override
        {
            (void)session;
            (void)channel;
            return true;
        }

        bool on_exec_request(SshSession *session,
                             SshChannel *channel,
                             const std::string &command) override
        {
            (void)session;
            (void)channel;
            (void)command;
            return true;
        }

        bool on_env_request(SshSession *session,
                            SshChannel *channel,
                            const std::string &name,
                            const std::string &value) override
        {
            (void)session;
            (void)channel;
            (void)name;
            (void)value;
            return true;
        }

        bool enable_builtin_exec_bridge() const override
        {
            return true;
        }
    };
}

#endif
