#include <cassert>

#include "net/acceptor/udp/kcp_adapter.h"
#include "base/time.h"
#include "net/connection/connection.h"

namespace yuan::net
{
    namespace
    {
        constexpr std::size_t kKcpDecodeBufferSize = 1472;
    }

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

    bool KcpAdapter::init(Connection * conn, timer::TimerManager * timerManager)
    {
        conn_ = conn;
        kcp_ = ikcp_create(conv, conn);
        kcp_->output = &KcpAdapter::on_send;
        ikcp_wndsize(kcp_, 128, 128);
        ikcp_nodelay(kcp_, 1, 10, 1, 1);
        updateTimer_ = timerManager->interval(0, timerManager->get_time_unit(), this, -1);
        return true;
    }

    int KcpAdapter::on_recv(yuan::buffer::ByteBuffer & buff)
    {
        int ret = ikcp_input(kcp_, buff.read_ptr(), static_cast<long>(buff.readable_bytes()));
        if (ret < 0) {
            return ret;
        }

        buff.clear();
        buff.ensure_writable(kKcpDecodeBufferSize);
        ret = ikcp_recv(kcp_, buff.write_ptr(), static_cast<int>(buff.writable_bytes()));
        if (ret < 0) {
            return ret;
        }

        buff.commit(static_cast<std::size_t>(ret));

        return ret;
    }

    int KcpAdapter::on_write(const yuan::buffer::ByteBuffer & buff)
    {
        return ikcp_send(kcp_, buff.read_ptr(), static_cast<int>(buff.readable_bytes()));
    }

    void KcpAdapter::on_release()
    {
        delete this;
    }

    int KcpAdapter::on_send(const char * buf, int len, ikcpcb * kcp, void * user)
    {
        assert(user && len > 0);
        Connection *conn = static_cast<Connection *>(user);
        conn->append_output(buf, len);
        conn->flush();
        return len;
    }

    void KcpAdapter::on_timer(timer::Timer * timer)
    {
        if (kcp_) {
            ikcp_update(kcp_, base::time::now());
        }
    }
}
