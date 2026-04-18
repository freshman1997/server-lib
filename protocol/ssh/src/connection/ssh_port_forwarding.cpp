#include "connection/ssh_port_forwarding.h"
#include "protocol/ssh_message_codec.h"
#include "ssh_handler.h"

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

        // In a real implementation, we'd call the handler to actually bind a port
        // For now, use the requested port as the allocated port
        entry.allocated_port = static_cast<uint16_t>(bind_port);
        allocated_port = entry.allocated_port;

        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            remote_forwards_.emplace(key, std::move(entry));
        }

        return true;
    }

    bool SshPortForwarding::handle_cancel_tcpip_forward(const std::string & bind_addr,
                                                        uint32_t bind_port)
    {
        std::string key = bind_addr + ":" + std::to_string(bind_port);

        {
            std::lock_guard<std::mutex> lock(remote_mutex_);
            return remote_forwards_.erase(key) > 0;
        }
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
