#ifndef __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__
#define __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__

#include <cstddef>

namespace yuan::net
{

struct ListenOptions
{
    bool reuse_addr = true;
    bool reuse_port = false;
#ifdef _WIN32
    bool exclusive_addr = true;
#else
    bool exclusive_addr = false;
#endif
    bool non_block = true;
    int backlog = 128;
    bool use_iocp = false;
    std::size_t iocp_worker_count = 1;
};

} // namespace yuan::net

#endif
