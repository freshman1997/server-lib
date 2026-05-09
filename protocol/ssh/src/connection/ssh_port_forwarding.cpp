#include "connection/ssh_port_forwarding.h"
#include "connection/ssh_connection_manager.h"
#include "connection/ssh_direct_tcpip_handler.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_handler.h"
#include "ssh_session.h"
#include "coroutine/accept_awaitable.h"
#include "coroutine/task.h"
#include "net/socket/inet_address.h"

#include <utility>

namespace yuan::net::ssh
{
    SshPortForwarding::SshPortForwarding(SshSession * session)
        : session_(session)
    {
    }

    bool SshPortForwarding::handle_tcpip_forward(const std::string & bind_addr,
                                                 uint32_t bind_port,
                                                 uint16_t & allocated_port)
    {
        std::string key = bind_addr + ":" + std::to_string(bind_port);

        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            if (remote_forwards_.count(key)) {
                return false;
            }
        }

        SshForwardEntry entry;
        entry.bind_addr = bind_addr;
        entry.bind_port = static_cast<uint16_t>(bind_port);

        entry.allocated_port = static_cast<uint16_t>(bind_port);
        allocated_port = entry.allocated_port;

        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            remote_forwards_.emplace(key, std::move(entry));
        }

        return true;
    }

    bool SshPortForwarding::attach_remote_forward_listener(const std::string & key,
                                                           std::shared_ptr<net::StreamAcceptor> acceptor)
    {
        if (!acceptor) {
            return false;
        }
        std::lock_guard<std::mutex> lock(remote_mutex_);
        auto it = remote_forwards_.find(key);
        if (it == remote_forwards_.end()) {
            return false;
        }
        it->second.acceptor = std::move(acceptor);
        it->second.accepting = false;
        return true;
    }

    bool SshPortForwarding::handle_cancel_tcpip_forward(const std::string & bind_addr,
                                                        uint32_t bind_port)
    {
        std::string key = bind_addr + ":" + std::to_string(bind_port);
        std::shared_ptr<net::StreamAcceptor> acceptor;
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            auto it = remote_forwards_.find(key);
            if (it != remote_forwards_.end()) {
                acceptor = it->second.acceptor;
                remote_forwards_.erase(it);
                removed = true;
            }
        }
        if (acceptor) {
            acceptor->close();
        }
        return removed;
    }

    void SshPortForwarding::poll_remote_forward_accepts()
    {
        struct AcceptStart
        {
            std::string key;
            std::shared_ptr<net::StreamAcceptor> acceptor;
        };
        std::vector<AcceptStart> to_start;
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            to_start.reserve(remote_forwards_.size());
            for (auto &item : remote_forwards_) {
                if (!item.second.acceptor || item.second.accepting) {
                    continue;
                }
                item.second.accepting = true;
                AcceptStart start;
                start.key = item.first;
                start.acceptor = item.second.acceptor;
                to_start.push_back(std::move(start));
            }
        }

        for (auto &start : to_start) {
            if (!start.acceptor || !runtime_.event_loop()) {
                std::lock_guard<std::mutex> lock(remote_mutex_);
                auto it = remote_forwards_.find(start.key);
                if (it != remote_forwards_.end()) {
                    it->second.accepting = false;
                }
                continue;
            }

            auto task = [this, key = start.key, acceptor = start.acceptor.get()]() -> coroutine::Task<void> {
                auto conn = co_await coroutine::async_accept(runtime_, acceptor);

                {
                    std::lock_guard<std::mutex> lock(remote_mutex_);
                    auto it = remote_forwards_.find(key);
                    if (it != remote_forwards_.end()) {
                        it->second.accepting = false;
                    }
                }

                if (!conn) {
                    co_return;
                }

                on_remote_forward_accept_ready(key, conn);
            }();
            task.resume();
            task.detach();
        }
    }

    void SshPortForwarding::on_remote_forward_accept_ready(const std::string & key,
                                                           const std::shared_ptr<net::Connection> & accepted_conn)
    {
        if (!session_ || !conn_mgr_ || !accepted_conn) {
            return;
        }

        std::string bind_addr;
        uint16_t bind_port = 0;
        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            auto it = remote_forwards_.find(key);
            if (it == remote_forwards_.end()) {
                accepted_conn->close();
                return;
            }
            bind_addr = it->second.bind_addr;
            bind_port = it->second.allocated_port;
        }

        auto accepted_remote = accepted_conn->get_remote_address();
        ByteBuffer open_packet;
        auto local_channel_id = conn_mgr_->open_forwarded_tcpip_channel(
            bind_addr,
            bind_port,
            accepted_remote.get_ip(),
            static_cast<uint32_t>(accepted_remote.get_port()),
            open_packet);
        if (!local_channel_id.has_value()) {
            accepted_conn->close();
            return;
        }

        auto handler = std::make_unique<SshDirectTcpipHandler>(
            session_,
            accepted_remote.get_ip(),
            static_cast<uint16_t>(accepted_remote.get_port()));
        if (!conn_mgr_->register_forwarded_tcpip_handler(*local_channel_id, std::move(handler))) {
            accepted_conn->close();
            return;
        }

        session_->enqueue_pending_forwarded_tcpip_open(std::move(open_packet));
    }

    ByteBuffer SshPortForwarding::handle_direct_tcpip(const SshChannelOpenMessage & msg, SshHandler * handler)
    {
        size_t offset = 0;
        auto host = SshMessageCodec::read_string(msg.type_specific_data.data(),
                                                 msg.type_specific_data.size(), offset);
        if (!host) {
            return ByteBuffer();
        }

        uint32_t port = 0;
        if (offset + 4 <= msg.type_specific_data.size()) {
            port = SshMessageCodec::read_uint32(msg.type_specific_data.data(),
                                                msg.type_specific_data.size(), offset);
        }

        auto orig_addr = SshMessageCodec::read_string(msg.type_specific_data.data(),
                                                      msg.type_specific_data.size(), offset);
        uint32_t orig_port = 0;
        if (offset + 4 <= msg.type_specific_data.size()) {
            orig_port = SshMessageCodec::read_uint32(msg.type_specific_data.data(),
                                                     msg.type_specific_data.size(), offset);
        }

        if (port == 0) {
            return ByteBuffer();
        }

        if (handler) {
            bool allowed = handler->on_direct_tcpip(session_, nullptr, *host, static_cast<uint16_t>(port));
            if (!allowed) {
                return ByteBuffer();
            }
        }

        SshForwardEntry entry;
        entry.bind_addr = *host;
        entry.bind_port = static_cast<uint16_t>(port);

        ByteBuffer result;
        SshMessageCodec::write_string(result, *host);
        SshMessageCodec::write_uint32(result, port);
        if (orig_addr) {
            SshMessageCodec::write_string(result, *orig_addr);
        } else {
            SshMessageCodec::write_string(result, "");
        }
        SshMessageCodec::write_uint32(result, orig_port);
        return result;
    }

    void SshPortForwarding::add_local_forward(uint32_t channel_id, SshForwardEntry entry)
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        local_forwards_.emplace(channel_id, std::move(entry));
    }

    void SshPortForwarding::remove_local_forward(uint32_t channel_id)
    {
        std::lock_guard<std::mutex> lock(local_mutex_);
        local_forwards_.erase(channel_id);
    }

    void SshPortForwarding::add_remote_forward(const std::string & key, SshForwardEntry entry)
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_forwards_.emplace(key, std::move(entry));
    }

    void SshPortForwarding::remove_remote_forward(const std::string & key)
    {
        std::lock_guard<std::mutex> lock(remote_mutex_);
        remote_forwards_.erase(key);
    }
}
