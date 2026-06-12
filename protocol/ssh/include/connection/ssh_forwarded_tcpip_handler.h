#ifndef __NET_SSH_CONNECTION_SSH_FORWARDED_TCPIP_HANDLER_H__
#define __NET_SSH_CONNECTION_SSH_FORWARDED_TCPIP_HANDLER_H__

#include "ssh_channel_handler.h"
#include "coroutine/runtime_view.h"
#include "coroutine/task.h"
#include "net/connection/connection.h"
#include <atomic>
#include <memory>

namespace yuan::net::ssh
{
    class SshChannel;
    class SshSession;

    class SshForwardedTcpipHandler final : public SshChannelHandler
    {
    public:
        SshForwardedTcpipHandler(SshSession *session,
                                 std::shared_ptr<net::Connection> accepted_conn);
        ~SshForwardedTcpipHandler() override;

        void on_open(SshChannel *channel) override;
        void on_data(SshChannel *channel, const std::vector<uint8_t> &data) override;
        void on_eof(SshChannel *channel) override;
        void on_close(SshChannel *channel) override;
        void on_window_adjust(SshChannel *channel, uint32_t bytes_to_add) override;

    private:
        struct SharedState
        {
            std::shared_ptr<net::Connection> accepted_conn;
            std::atomic<bool> closed{ false };
        };

        static coroutine::Task<void> relay_from_accepted(SshSession *session,
                                                         uint32_t local_channel_id,
                                                         std::shared_ptr<SharedState> state);

        SshSession *session_;
        std::shared_ptr<SharedState> state_;
    };
}

#endif
