#ifndef __NET_BASE_ACCEPTOR_UDP_KCP_ADAPTER_H__
#define __NET_BASE_ACCEPTOR_UDP_KCP_ADAPTER_H__
#include "ikcp.h"
#include "net/acceptor/udp/adapter.h"
#include "net/connection/connection.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "timer/timer_task.h"

namespace net 
{
    class KcpAdapter : public timer::TimerTask, public UdpAdapter
    {
    public:
        KcpAdapter();
        ~KcpAdapter();

    public:
        virtual bool init(Connection *conn, timer::TimerManager *timerManager);
        virtual bool on_recv();
        virtual int on_write();
        virtual void on_release();

    public:
        static int on_send(const char *buf, int len, ikcpcb *kcp, void *user);

    public:
        virtual void on_timer(timer::Timer *timer);

    private:
        int conv;
        ikcpcb *kcp_;
        Connection *conn_;
        timer::Timer *updateTimer_;
    };
}

#endif