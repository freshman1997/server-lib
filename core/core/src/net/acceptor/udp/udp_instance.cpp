#include "net/acceptor/udp/udp_instance.h"
#include "net/acceptor/udp/kcp_adapter.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/connection/datagram_transport.h"
#include "net/acceptor/udp/adapter.h"
#include "net/handler/connection_handler.h"
#include <cassert>
#include <utility>

namespace yuan::net
{
    UdpInstance::UdpInstance(DatagramEndpoint * acceptor)
        : acceptor_(acceptor), adapter_type_(UdpAdapterType::none), is_closing_(false)
    {
    }

    UdpInstance::~UdpInstance()
    {
        is_closing_ = true;
        for (auto &it : conns_) {
            if (it.second) {
                it.second->abort();
            }
        }
        conns_.clear();
    }

    void UdpInstance::set_acceptor(DatagramEndpoint * acceptor)
    {
        acceptor_ = acceptor;
    }

    std::pair<bool, std::shared_ptr<Connection>> UdpInstance::on_recv(const InetAddress & address)
    {
        auto it = conns_.find(address);
        if (it == conns_.end()) {
            std::shared_ptr<Connection> udpConn;
            if (adapter_type_ == UdpAdapterType::kcp) {
                UdpAdapter *adapter = new KcpAdapter;
                udpConn = create_datagram_connection(address, adapter);
                if (!adapter->init(udpConn ? &*udpConn : nullptr, acceptor_->endpoint_timer_manager())) {
                    return { false, udpConn };
                }
            } else {
                udpConn = create_datagram_connection(address);
            }
            if (auto datagram = std::dynamic_pointer_cast<DatagramTransport>(udpConn)) {
                datagram->attach_datagram_instance(this);
            }
            conns_[address] = udpConn;
            metrics_.created_connections++;
            metrics_.active_connections = conns_.size();
            return std::make_pair(true, udpConn);
        } else {
            return std::make_pair(true, it->second);
        }
    }

    int UdpInstance::on_send(Connection * conn, const yuan::buffer::ByteBuffer & buff)
    {
        assert(acceptor_);
        return acceptor_->send_datagram(conn ? conn->shared_from_this() : nullptr, buff);
    }

    void UdpInstance::account_read(std::size_t bytes)
    {
        metrics_.datagrams_read++;
        metrics_.bytes_read += bytes;
    }

    void UdpInstance::account_drop(std::size_t bytes)
    {
        metrics_.datagrams_dropped++;
        metrics_.bytes_dropped += bytes;
    }

    void UdpInstance::account_send(std::size_t bytes)
    {
        metrics_.datagrams_written++;
        metrics_.bytes_written += bytes;
    }

    void UdpInstance::account_send_error()
    {
        metrics_.send_errors++;
    }

    void UdpInstance::account_receive_error()
    {
        metrics_.receive_errors++;
    }

    UdpConnectionMetrics UdpInstance::metrics() const
    {
        auto metrics = metrics_;
        metrics.active_connections = conns_.size();
        return metrics;
    }

    void UdpInstance::send()
    {
        if (!pending_write_addrs_.empty()) {
            while (!pending_write_addrs_.empty()) {
                auto address = std::move(pending_write_addrs_.front());
                pending_write_addrs_.pop_front();
                pending_write_set_.erase(address);

                auto current = conns_.find(address);
                if (current == conns_.end() || !current->second) {
                    continue;
                }

                if (current->second->is_connected()) {
                    current->second->on_write_event();
                } else {
                    current->second->flush();
                }
            }
            return;
        }

        auto it = conns_.begin();
        while (it != conns_.end()) {
            auto current = it;
            ++it; // advance before processing, as on_write_event/flush may delete the connection
            if (current->second->is_connected()) {
                current->second->on_write_event();
            } else {
                current->second->flush();
            }
        }
    }

    void UdpInstance::on_connection_close(const std::shared_ptr<Connection> &conn)
    {
        on_connection_close_raw(conn.get());
    }

    void UdpInstance::on_connection_close_raw(Connection *conn)
    {
        if (is_closing_ || !conn) {
            return;
        }

        auto it = conns_.find(conn->get_remote_address());
        if (it != conns_.end()) {
            pending_write_set_.erase(it->first);
            conns_.erase(it);
            metrics_.closed_connections++;
            metrics_.active_connections = conns_.size();
        }
    }

    timer::TimerManager *UdpInstance::get_timer_manager() const
    {
        return acceptor_ ? acceptor_->endpoint_timer_manager() : nullptr;
    }

    void UdpInstance::enable_rw_events()
    {
        if (acceptor_) {
            acceptor_->endpoint_channel()->enable_read();
            if (!pending_write_addrs_.empty()) {
                acceptor_->endpoint_channel()->enable_write();
            }
            acceptor_->update_endpoint_channel();
        }
    }

    void UdpInstance::request_write(Connection *conn)
    {
        if (conn) {
            const auto &address = conn->get_remote_address();
            if (pending_write_set_.insert(address).second) {
                pending_write_addrs_.push_back(address);
            }
        }
        enable_rw_events();
    }
}
