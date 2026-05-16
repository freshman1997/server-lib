#ifndef __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__
#define __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__

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
};

} // namespace yuan::net

#endif
