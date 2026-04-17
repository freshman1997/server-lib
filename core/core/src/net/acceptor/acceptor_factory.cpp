#include "coroutine/runtime.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/runtime/network_runtime.h"

namespace yuan::net
{

    StreamAcceptor *create_stream_acceptor(Socket * socket)
    {
        return new TcpAcceptor(socket);
    }

    DatagramAcceptor *create_datagram_acceptor(Socket * socket, yuan::timer::TimerManager * timer_manager)
    {
        return new UdpAcceptor(socket, timer_manager);
    }

    DatagramAcceptor *create_datagram_acceptor(Socket * socket, coroutine::RuntimeView runtime)
    {
        return new UdpAcceptor(socket, runtime.timer_manager());
    }

    DatagramAcceptor *create_datagram_acceptor(Socket * socket, NetworkRuntime & runtime)
    {
        return new UdpAcceptor(socket, runtime.timer_manager());
    }

} // namespace yuan::net
