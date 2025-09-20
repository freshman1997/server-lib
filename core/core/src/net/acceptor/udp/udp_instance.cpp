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

namespace yuan::net 
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
            udpConn->set_instance_handler(this);
            conns_[address] = udpConn;
            return {true, udpConn};
        } else {
            return {true, it->second};
        }
    }

    int UdpInstance::on_send(Connection *conn, buffer::Buffer *buff)
    {
        assert(acceptor_);
        return acceptor_->send_to(conn, buff);
    }

    void UdpInstance::send()
    {
        try_free_connections();
        for (auto it = conns_.begin(); it != conns_.end(); ++it) {
            if (it->second->is_connected()) {
                it->second->on_write_event();
            } else {
                it->second->flush();
            }
        }
        try_free_connections();
    }

    void UdpInstance::on_connection_close(Connection *conn)
    {
        if (is_closing_) {
            return;
        }

        auto it = conns_.find(conn->get_remote_address());
        if (it == conns_.end()) {
            return;
        }
        free_addrs_.insert(it->first);
    }

    timer::TimerManager * UdpInstance::get_timer_manager()
    {
        return acceptor_ ? acceptor_->get_timer_manager() : nullptr;
    }

    void UdpInstance::enable_rw_events()
    {
        if (acceptor_) {
            acceptor_->get_channel()->enable_read();
            acceptor_->get_channel()->enable_write();
            acceptor_->update_channel();
        }
    }

    void UdpInstance::try_free_connections()
    {
        if (!free_addrs_.empty()) {
            for (const auto &addr : free_addrs_) {
                auto it = conns_.find(addr);
                if (it != conns_.end()) {
                    conns_.erase(it);
                }
            }
            free_addrs_.clear();
        }
    }
}