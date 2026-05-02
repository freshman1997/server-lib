#include "connection/ssh_channel.h"
#include <algorithm>

namespace yuan::net::ssh
{
    SshChannel::SshChannel(uint32_t local_id, const std::string & channel_type,
                           uint32_t initial_window, uint32_t max_packet_size)
        : local_id_(local_id), channel_type_(channel_type),
          local_window_(initial_window), remote_window_(0),
          local_max_packet_(max_packet_size)
    {
    }

    void SshChannel::consume_local_window(uint32_t bytes)
    {
        uint32_t cur = local_window_.load(std::memory_order_relaxed);
        if (bytes > cur)
            bytes = cur;
        local_window_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    void SshChannel::adjust_local_window(uint32_t bytes_to_add)
    {
        local_window_.fetch_add(bytes_to_add, std::memory_order_relaxed);
    }

    void SshChannel::consume_remote_window(uint32_t bytes)
    {
        uint32_t cur = remote_window_.load(std::memory_order_relaxed);
        if (bytes > cur)
            bytes = cur;
        remote_window_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    void SshChannel::adjust_remote_window(uint32_t bytes_to_add)
    {
        remote_window_.fetch_add(bytes_to_add, std::memory_order_relaxed);
    }

    bool SshChannel::local_window_available(uint32_t bytes) const
    {
        return local_window_.load(std::memory_order_relaxed) >= bytes;
    }

    bool SshChannel::remote_window_available(uint32_t bytes) const
    {
        return remote_window_.load(std::memory_order_relaxed) >= bytes;
    }

    void SshChannel::enqueue_data(std::vector<uint8_t> data)
    {
        pending_total_ += static_cast<uint32_t>(data.size());
        pending_data_.push_back(std::move(data));
    }

    bool SshChannel::has_pending_data() const
    {
        return !pending_data_.empty();
    }

    std::vector<uint8_t> SshChannel::dequeue_pending(uint32_t max_bytes)
    {
        if (pending_data_.empty())
            return {};

        auto &front = pending_data_.front();
        if (front.size() <= max_bytes) {
            auto result = std::move(front);
            pending_total_ -= static_cast<uint32_t>(result.size());
            pending_data_.pop_front();
            return result;
        }

        std::vector<uint8_t> result(front.begin(), front.begin() + max_bytes);
        front.erase(front.begin(), front.begin() + max_bytes);
        pending_total_ -= max_bytes;
        return result;
    }

    bool SshChannel::mark_command_started()
    {
        if (command_started_) {
            return false;
        }
        command_started_ = true;
        return true;
    }

    bool SshChannel::mark_pty_requested()
    {
        if (pty_requested_) {
            return false;
        }
        pty_requested_ = true;
        return true;
    }

    bool SshChannel::mark_termination_notified()
    {
        if (termination_notified_) {
            return false;
        }
        termination_notified_ = true;
        return true;
    }
}
