#ifndef __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__
#define __YUAN_NET_SOCKET_LISTEN_OPTIONS_H__

#include <cstddef>

namespace yuan::net
{

enum class ListenSchedulingMode
{
    throughput,
    affinity
};

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
    ListenSchedulingMode scheduling_mode = ListenSchedulingMode::throughput;
    std::size_t shard_count = 1;
    std::size_t max_connections = 0;
    std::size_t max_connections_per_ip = 0;
    std::size_t max_input_buffer_bytes = 0;
    std::size_t max_output_buffer_bytes = 0;
    std::size_t iocp_worker_count = 1;
    std::size_t iocp_completion_batch_size = 1;
};

} // namespace yuan::net

#endif
