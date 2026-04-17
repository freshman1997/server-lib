#ifndef __NET_SSH_CONNECTION_SSH_GLOBAL_REQUEST_H__
#define __NET_SSH_CONNECTION_SSH_GLOBAL_REQUEST_H__

#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshGlobalRequestHandler
    {
    public:
        virtual ~SshGlobalRequestHandler() = default;

        virtual bool handle_tcpip_forward(const std::string &bind_addr,
                                          uint32_t bind_port,
                                          uint16_t &allocated_port) = 0;

        virtual bool handle_cancel_tcpip_forward(const std::string &bind_addr,
                                                 uint32_t bind_port) = 0;
    };
}

#endif
