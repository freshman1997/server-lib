#include <cassert>

#include "net/acceptor/udp/kcp_adapter.h"
#include "base/time.h"
#include "buffer/buffer.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"

namespace yuan::net 
{
    KcpAdapter::KcpAdapter()
    {
        conv = 0x23fedacd;
        kcp_ = nullptr;
        conn_ = nullptr;
        updateTimer_ = nullptr;
    }

    KcpAdapter::~KcpAdapter()
    {
        if (kcp_) {
            ikcp_release(kcp_);
            kcp_ = nullptr;
        }

        if (updateTimer_) {
            updateTimer_->cancel();
            updateTimer_ = nullptr;
        }
    }

    bool KcpAdapter::init(Connection *conn, timer::TimerManager *timerManager)
    {
        conn_ = conn;
        kcp_ = ikcp_create(conv, conn);
        kcp_->output = &KcpAdapter::on_send;
        ikcp_nodelay(kcp_, 1, 10, 1, 1);
        updateTimer_ = timerManager->interval(0, timerManager->get_time_unit(), this, -1);
        return true;
    }

    bool KcpAdapter::on_recv()
    {
        assert(conn_);
        bool ok = false;
        conn_->process_input_data([this, &ok](buffer::Buffer *buff) -> bool {
            int ret = ikcp_input(kcp_, buff->peek(), buff->readable_bytes());
            if (ret < 0) {
                ok = false;
                return false;
            }

            buff->reset();
            int n = ikcp_recv(kcp_, buff->begin(), buff->writable_size());
            if (n > 0) {
                buff->fill(n);
            }
            ok = true;
            return true;
        }, false);

        return ok;
    }

    int KcpAdapter::on_write()
    {
        assert(conn_);
        auto buff = conn_->get_output_buff();
        return ikcp_send(kcp_, buff->peek(), buff->readable_bytes());
    }

    void KcpAdapter::on_release()
    {
        delete this;
    }

    int KcpAdapter::on_send(const char *buf, int len, ikcpcb *kcp, void *user)
    {
        assert(user && len > 0);
        Connection *conn = static_cast<Connection *>(user);
        auto buff = conn->get_output_buff();
        buff->write_string(buf, len);
        return len;
    }

    void KcpAdapter::on_timer(timer::Timer *timer)
    {
        if (kcp_) {
            ikcp_update(kcp_, base::time::now());
        }
    }
}