#ifndef __NET_SSH_CONNECTION_SSH_PORT_FORWARDING_H__
#define __NET_SSH_CONNECTION_SSH_PORT_FORWARDING_H__

#include "connection/ssh_global_request.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    class SshSession;
    class SshHandler;

    struct SshForwardEntry
    {
        std::string bind_addr;
        uint16_t bind_port = 0;
        uint16_t allocated_port = 0;
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

        void add_local_forward(uint32_t channel_id, SshForwardEntry entry);
        void remove_local_forward(uint32_t channel_id);
        void add_remote_forward(const std::string &key, SshForwardEntry entry);
        void remove_remote_forward(const std::string &key);

    private:
        SshSession *session_;

        mutable std::mutex local_mutex_;
        std::unordered_map<uint32_t, SshForwardEntry> local_forwards_;

        mutable std::mutex remote_mutex_;
        std::unordered_map<std::string, SshForwardEntry> remote_forwards_;
    };
}

#endif
