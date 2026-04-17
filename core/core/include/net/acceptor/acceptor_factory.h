#ifndef __YUAN_NET_ACCEPTOR_FACTORY_H__
#define __YUAN_NET_ACCEPTOR_FACTORY_H__

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
    class StreamAcceptor;
    class DatagramAcceptor;
    class NetworkRuntime;

    StreamAcceptor *create_stream_acceptor(Socket * socket);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, yuan::timer::TimerManager * timer_manager);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, coroutine::RuntimeView runtime);
    DatagramAcceptor *create_datagram_acceptor(Socket * socket, NetworkRuntime & runtime);

} // namespace yuan::net

#endif
