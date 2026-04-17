#ifndef __NET_SSH_SSH_CHANNEL_HANDLER_H__
#define __NET_SSH_SSH_CHANNEL_HANDLER_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshChannel;

    class SshChannelHandler
    {
    public:
        virtual ~SshChannelHandler() = default;

        virtual void on_open(SshChannel *channel)
        {
        }

        virtual void on_data(SshChannel *channel,
                             const std::vector<uint8_t> &data)
        {
        }

        virtual void on_eof(SshChannel *channel)
        {
        }

        virtual void on_close(SshChannel *channel)
        {
        }

        virtual void on_window_adjust(SshChannel *channel,
                                      uint32_t bytes_to_add)
        {
        }

        virtual bool on_request(SshChannel *channel,
                                const std::string &request_type,
                                const std::vector<uint8_t> &request_data)
        {
            return false;
        }
    };
}

#endif
