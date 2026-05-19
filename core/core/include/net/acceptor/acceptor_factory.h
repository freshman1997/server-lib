#ifndef __YUAN_NET_ACCEPTOR_FACTORY_H__
#define __YUAN_NET_ACCEPTOR_FACTORY_H__

#include <cstdint>
#include <string>

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::coroutine
{
    class RuntimeView;
}

namespace yuan::net
{

    class Socket;
    struct ListenOptions;
    class StreamAcceptor;
    class DatagramAcceptor;
    class NetworkRuntime;

    StreamAcceptor *create_stream_acceptor(Socket * socket);
    StreamAcceptor *create_iocp_stream_acceptor(const std::string &host,
                                                uint16_t port,
                                                NetworkRuntime &runtime,
                                                const ListenOptions &options);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, yuan::timer::TimerManager * timer_manager);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, coroutine::RuntimeView runtime);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, NetworkRuntime & runtime);

} // namespace yuan::net

#endif
