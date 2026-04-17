#ifndef __NET_SSH_SSH_HANDLER_H__
#define __NET_SSH_SSH_HANDLER_H__

#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshSession;
    class SshChannel;

    class SshHandler
    {
    public:
        virtual ~SshHandler() = default;

        virtual SshAuthResult on_authenticate(SshSession *session,
                                              const std::string &username,
                                              const std::string &method,
                                              const SshAuthCredentials &credentials)
        {
            return SshAuthResult::FAILURE;
        }

        virtual bool on_channel_open(SshSession *session,
                                     const std::string &channel_type,
                                     SshChannel *channel)
        {
            return true;
        }

        virtual void on_channel_close(SshSession *session,
                                      SshChannel *channel)
        {
        }

        virtual void on_channel_data(SshSession *session,
                                     SshChannel *channel,
                                     const std::vector<uint8_t> &data)
        {
        }

        virtual bool on_channel_request(SshSession *session,
                                        SshChannel *channel,
                                        const std::string &request_type,
                                        const std::vector<uint8_t> &request_data)
        {
            return false;
        }

        virtual bool on_global_request(SshSession *session,
                                       const std::string &request_name,
                                       const std::vector<uint8_t> &request_data)
        {
            return false;
        }

        virtual void on_session_opened(SshSession *session)
        {
        }

        virtual void on_session_closed(SshSession *session)
        {
        }

        virtual uint16_t on_tcpip_forward(SshSession *session,
                                          const std::string &bind_addr,
                                          uint16_t bind_port)
        {
            return 0;
        }

        virtual void on_cancel_tcpip_forward(SshSession *session,
                                             const std::string &bind_addr,
                                             uint16_t bind_port)
        {
        }

        virtual bool on_direct_tcpip(SshSession *session,
                                     SshChannel *channel,
                                     const std::string &target_host,
                                     uint16_t target_port)
        {
            return true;
        }

        virtual bool on_pty_request(SshSession *session,
                                    SshChannel *channel,
                                    const std::string &term,
                                    uint32_t width, uint32_t height,
                                    uint32_t pixel_width, uint32_t pixel_height,
                                    const std::vector<uint8_t> &modes)
        {
            return false;
        }

        virtual bool on_shell_request(SshSession *session,
                                      SshChannel *channel)
        {
            return false;
        }

        virtual bool on_exec_request(SshSession *session,
                                     SshChannel *channel,
                                     const std::string &command)
        {
            return false;
        }

        virtual bool on_subsystem_request(SshSession *session,
                                          SshChannel *channel,
                                          const std::string &name)
        {
            return false;
        }

        virtual bool on_env_request(SshSession *session,
                                    SshChannel *channel,
                                    const std::string &name,
                                    const std::string &value)
        {
            return false;
        }

        virtual void on_window_change(SshSession *session,
                                      SshChannel *channel,
                                      uint32_t width, uint32_t height,
                                      uint32_t pixel_width, uint32_t pixel_height)
        {
        }

        virtual void on_signal(SshSession *session,
                               SshChannel *channel,
                               const std::string &signal_name)
        {
        }

        virtual bool on_x11_forward(SshSession *session,
                                    SshChannel *channel,
                                    const std::string &auth_protocol,
                                    const std::string &auth_cookie,
                                    uint32_t screen_number)
        {
            return false;
        }

        virtual bool on_agent_forward(SshSession *session,
                                      SshChannel *channel)
        {
            return false;
        }
    };
}

#endif
