#include "net/base/acceptor/udp/udp_instance.h"
#include "buffer/pool.h"
#include "net/base/acceptor/udp/kcp_adapter.h"
#include "net/base/acceptor/udp_acceptor.h"
#include "net/base/connection/connection.h"
#include "net/base/connection/udp_connection.h"
#include "singleton/singleton.h"
#include "net/base/acceptor/udp/adapter.h"
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

        for (auto it : queue_) {
            singleton::Singleton<BufferedPool>().free(it.second);
        }
        queue_.clear();
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
                    udpConn->abort();
                    return {false, nullptr};
                }
            } else {
                udpConn = new UdpConnection(address);
            }

            conns_[address] = udpConn;

            return {true, udpConn};
        } else {
            return {false, it->second};
        }
    }

    void UdpInstance::on_send(Connection *conn, Buffer *buff)
    {
        queue_.push_back({conn, buff});
    }

    void UdpInstance::send()
    {
        if (!queue_.empty()) {
            const auto &item = queue_.front();
            int ret = acceptor_->send_to(item.first, item.second);
            if (ret < 0 || ret >= item.second->readable_bytes()) {
                queue_.pop_front();
            }
        }
    }

    void UdpInstance::on_connection_close(Connection *conn)
    {
        if (is_closing_) {
            return;
        }

        // TODO 有待优化
        conns_.erase(conn->get_remote_address());
        for (auto it = queue_.begin(); it != queue_.end(); ) {
            if (it->first == conn) {
                singleton::Singleton<BufferedPool>().free(it->second);
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
    }

    timer::TimerManager * UdpInstance::get_timer_manager()
    {
        return acceptor_->get_timer_manager();
    }
}