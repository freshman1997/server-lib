#ifndef __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
#define __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
#include "buffer/byte_buffer_reader.h"
#include "cmd/multi_cmd.h"
#include "cmd/subcribe_cmd.h"
#include "coroutine/completion_event.h"
#include "internal/coroutine.h"
#include "internal/redis_registry.h"
#include "option.h"
#include "redis_client.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace yuan::redis
{
    enum class RedisState : uint8_t {
        connecting = 1,
        connected = 2,
        timeout = 4,
        disconnecting = 5,
        closed = 6,
    };
    class RedisClient::Impl final : public net::ConnectionHandler
    {
    public:
        using Ptr = std::shared_ptr<Impl>;

        friend class Psub;

    public:
        Impl()
        {
            clear_mask();
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        Impl(Impl &&) = delete;
        Impl &operator=(Impl &&) = delete;
        ~Impl() override;

    public:
        void on_connected(const std::shared_ptr<net::Connection> &conn) override;

        void on_error(const std::shared_ptr<net::Connection> &conn) override;

    public:
        void on_read(const std::shared_ptr<net::Connection> &conn) override;

        void on_write(const std::shared_ptr<net::Connection> &conn) override;

        void on_close(const std::shared_ptr<net::Connection> &conn) override;

    public:
        void on_do_connect(std::shared_ptr<net::Connection> conn);

        void close();
        void disconnect();

        void set_mask(RedisState state, const bool only = false)
        {
            if (only)
                clear_mask();
            mask_.fetch_or(1 << static_cast<uint8_t>(state), std::memory_order_release);
        }

        void clear_mask(RedisState state)
        {
            mask_.fetch_and(~(1 << static_cast<uint8_t>(state)), std::memory_order_release);
        }

        void clear_mask()
        {
            mask_.store(0, std::memory_order_release);
        }

        bool is_connecting() const
        {
            return mask_.load(std::memory_order_acquire) & 1 << static_cast<uint8_t>(RedisState::connecting);
        }

        bool is_connected() const
        {
            return mask_.load(std::memory_order_acquire) & 1 << static_cast<uint8_t>(RedisState::connected);
        }

        bool is_closed() const
        {
            return mask_.load(std::memory_order_acquire) & 1 << static_cast<uint8_t>(RedisState::closed);
        }

        bool is_disconnecting() const
        {
            return mask_.load(std::memory_order_acquire) & 1 << static_cast<uint8_t>(RedisState::disconnecting);
        }

        bool is_timeout() const
        {
            return mask_.load(std::memory_order_acquire) & 1 << static_cast<uint8_t>(RedisState::timeout);
        }

        std::shared_ptr<RedisValue> execute_command(std::shared_ptr<Command> cmd);

        RedisRegistry *registry() const;

        buffer::ByteBufferReader *get_reader()
        {
            return &reader_;
        }

        int fetch_next_message(int timeout);

    public:
        std::atomic<uint8_t> mask_ = 0;
        RedisClient *client_ = nullptr;
        std::shared_ptr<RedisRegistry> registry_;
        Option option_;
        std::shared_ptr<net::Connection> conn_;
        std::shared_ptr<Command> last_cmd_;
        std::shared_ptr<MultiCmd> multi_cmd_;
        std::shared_ptr<SubcribeCmd> subcribe_cmd;
        std::atomic<std::shared_ptr<RedisValue>> last_error_;
        buffer::ByteBufferReader reader_;
        yuan::coroutine::CompletionEvent completion_event_;
        std::recursive_mutex operation_mutex_;
        std::atomic<bool> closed_by_user_{false};
        std::atomic<int> in_flight_{0};
        std::atomic<std::uint64_t> reconnect_attempts_{0};
        std::atomic<std::uint64_t> reconnect_successes_{0};
        std::atomic<std::uint64_t> command_timeouts_{0};
        std::atomic<std::uint64_t> protocol_errors_{0};
    };
}

#endif // __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
