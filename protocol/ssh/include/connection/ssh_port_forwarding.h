#ifndef __NET_SSH_CONNECTION_SSH_PORT_FORWARDING_H__
#define __NET_SSH_CONNECTION_SSH_PORT_FORWARDING_H__

#include "connection/ssh_global_request.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include "buffer/byte_buffer.h"
#include "coroutine/runtime_view.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/connection/connection.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    using ::yuan::buffer::ByteBuffer;

    class SshSession;
    class SshHandler;
    class SshConnectionManager;

    struct SshForwardEntry
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
        uint16_t allocated_port = 0;
        std::shared_ptr<net::StreamAcceptor> acceptor;
        bool accepting = false;
    };

    class SshPortForwarding : public SshGlobalRequestHandler
    {
    public:
        explicit SshPortForwarding(SshSession *session);

        bool handle_tcpip_forward(const std::string &bind_addr,
                                  uint32_t bind_port,
                                  uint16_t &allocated_port) override;

        bool handle_cancel_tcpip_forward(const std::string &bind_addr,
                                         uint32_t bind_port) override;

        ByteBuffer handle_direct_tcpip(const SshChannelOpenMessage &msg, SshHandler *handler);

        void set_connection_manager(SshConnectionManager *conn_mgr)
        {
            conn_mgr_ = conn_mgr;
        }

        void set_runtime(coroutine::RuntimeView runtime)
        {
            runtime_ = runtime;
        }

        void poll_remote_forward_accepts();
        bool has_remote_forwards() const;
        bool attach_remote_forward_listener(const std::string &key,
                                            std::shared_ptr<net::StreamAcceptor> acceptor);

        void add_local_forward(uint32_t channel_id, SshForwardEntry entry);
        void remove_local_forward(uint32_t channel_id);
        void add_remote_forward(const std::string &key, SshForwardEntry entry);
        void remove_remote_forward(const std::string &key);

    private:
        void on_remote_forward_accept_ready(const std::string &key,
                                            const std::shared_ptr<net::Connection> &accepted_conn);

        SshSession *session_;
        SshConnectionManager *conn_mgr_ = nullptr;
        coroutine::RuntimeView runtime_;

        mutable std::mutex local_mutex_;
        std::unordered_map<uint32_t, SshForwardEntry> local_forwards_;

        mutable std::mutex remote_mutex_;
        std::unordered_map<std::string, SshForwardEntry> remote_forwards_;
    };
}

#endif
