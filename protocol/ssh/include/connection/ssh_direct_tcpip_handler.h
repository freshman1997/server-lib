#ifndef __NET_SSH_CONNECTION_SSH_DIRECT_TCPIP_HANDLER_H__
#define __NET_SSH_CONNECTION_SSH_DIRECT_TCPIP_HANDLER_H__

#include "ssh_channel_handler.h"
#include "coroutine/runtime_view.h"
#include "coroutine/task.h"
#include "net/connection/connection.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshChannel;
    class SshSession;

    class SshDirectTcpipHandler : public SshChannelHandler
    {
    public:
        SshDirectTcpipHandler(SshSession *session,
                              const std::string &target_host,
                              uint16_t target_port);
        ~SshDirectTcpipHandler() override;

        void on_open(SshChannel *channel) override;
        void on_data(SshChannel *channel, const std::vector<uint8_t> &data) override;
        void on_eof(SshChannel *channel) override;
        void on_close(SshChannel *channel) override;
        void on_window_adjust(SshChannel *channel, uint32_t bytes_to_add) override;

    private:
        coroutine::Task<void> relay_from_target(SshChannel *channel);

        SshSession *session_;
        std::string target_host_;
        uint16_t target_port_;
        std::shared_ptr<net::Connection> target_conn_;
        std::atomic<bool> relay_active_{ false };
        std::atomic<bool> closed_{ false };
    };
}

#endif
