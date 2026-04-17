#ifndef __NET_SSH_CONNECTION_SSH_CHANNEL_H__
#define __NET_SSH_CONNECTION_SSH_CHANNEL_H__

#include "protocol/ssh_constants.h"
#include "ssh_channel_handler.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshChannel
    {
    public:
        enum class State {
            closed,
            opening,
            open,
            eof,
            closing
        };

        SshChannel(uint32_t local_id, const std::string &channel_type,
                   uint32_t initial_window, uint32_t max_packet_size);
        ~SshChannel() = default;

        uint32_t local_id() const
        {
            return local_id_;
        }
        uint32_t remote_id() const
        {
            return remote_id_;
        }
        const std::string &channel_type() const
        {
            return channel_type_;
        }
        State state() const
        {
            return state_;
        }

        uint32_t local_window() const
        {
            return local_window_.load(std::memory_order_relaxed);
        }
        uint32_t remote_window() const
        {
            return remote_window_.load(std::memory_order_relaxed);
        }
        uint32_t local_max_packet() const
        {
            return local_max_packet_;
        }
        uint32_t remote_max_packet() const
        {
            return remote_max_packet_;
        }

        void set_remote_id(uint32_t id)
        {
            remote_id_ = id;
        }
        void set_remote_window(uint32_t w)
        {
            remote_window_.store(w, std::memory_order_relaxed);
        }
        void set_remote_max_packet(uint32_t s)
        {
            remote_max_packet_ = s;
        }
        void set_state(State s)
        {
            state_ = s;
        }
        void set_handler(std::unique_ptr<SshChannelHandler> h)
        {
            handler_ = std::move(h);
        }
        SshChannelHandler *handler() const
        {
            return handler_.get();
        }

        void consume_local_window(uint32_t bytes);
        void adjust_local_window(uint32_t bytes_to_add);
        void consume_remote_window(uint32_t bytes);
        void adjust_remote_window(uint32_t bytes_to_add);

        bool local_window_available(uint32_t bytes) const;
        bool remote_window_available(uint32_t bytes) const;

        void enqueue_data(std::vector<uint8_t> data);
        bool has_pending_data() const;
        std::vector<uint8_t> dequeue_pending(uint32_t max_bytes);

    private:
        uint32_t local_id_;
        uint32_t remote_id_ = 0;
        std::string channel_type_;
        State state_ = State::opening;

        std::atomic<uint32_t> local_window_;
        std::atomic<uint32_t> remote_window_;
        uint32_t local_max_packet_;
        uint32_t remote_max_packet_ = SSH_DEFAULT_MAX_PACKET_SIZE;

        std::unique_ptr<SshChannelHandler> handler_;

        std::deque<std::vector<uint8_t> > pending_data_;
        uint32_t pending_total_ = 0;
    };
}

#endif
