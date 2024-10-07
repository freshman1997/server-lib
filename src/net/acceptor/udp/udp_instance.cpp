#include "net/acceptor/udp/udp_instance.h"
#include "buffer/pool.h"
#include "net/acceptor/udp/kcp_adapter.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/connection/connection.h"
#include "net/connection/udp_connection.h"
#include "net/acceptor/udp/adapter.h"
#include "net/handler/connection_handler.h"
#include <cassert>
#include <utility>

namespace net 
{
    UdpInstance::UdpInstance(UdpAcceptor *acceptor) : acceptor_(acceptor), adapter_type_(UdpAdapterType::none), is_closing_(false)
    {
    }

    UdpInstance::~UdpInstance()
    {
        is_closing_ = true;
        for (auto it : conns_) {
            it.second->abort();
        }
        conns_.clear();
    }

    void UdpInstance::set_acceptor(UdpAcceptor *acceptor)
    {
        acceptor_ = acceptor;
    }

    std::pair<bool, Connection *> UdpInstance::on_recv(const InetAddress &address)
    {
        auto it = conns_.find(address);
        if (it == conns_.end()) {
            UdpConnection *udpConn;
            if (adapter_type_ == UdpAdapterType::kcp) {
                UdpAdapter *adapter = new KcpAdapter;
                udpConn = new UdpConnection(address, adapter);
                if (!adapter->init(udpConn, acceptor_->get_timer_manager())) {
                    return {false, udpConn};
                }
            } else {
                udpConn = new UdpConnection(address);
            }
            conns_[address] = udpConn;
            return {true, udpConn};
        } else {
            return {true, it->second};
        }
    }

    int UdpInstance::on_send(Connection *conn, Buffer *buff)
    {
        assert(acceptor_);
        return acceptor_->send_to(conn, buff);
    }

    void UdpInstance::send()
    {
        for (auto &item : conns_) {
            item.second->on_write_event();
        }
    }

    void UdpInstance::on_connection_close(Connection *conn)
    {
        if (is_closing_) {
            return;
        }
        conns_.erase(conn->get_remote_address());
    }

    timer::TimerManager * UdpInstance::get_timer_manager()
    {
        return acceptor_->get_timer_manager();
    }
}