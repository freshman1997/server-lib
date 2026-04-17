#ifndef __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
#define __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
#include "buffer/byte_buffer_reader.h"
#include "cmd/multi_cmd.h"
#include "cmd/subcribe_cmd.h"
#include "coroutine/completion_event.h"
#include "internal/coroutine.h"
#include "option.h"
#include "redis_client.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "timer/timer_task.h"

#include <memory>

namespace yuan::redis
{
    enum class RedisState : uint8_t {
        connecting = 1,
        connected = 2,
        timeout = 4,
        disconnecting = 5,
        closed = 6,
    };
    class RedisClient::Impl final : public net::ConnectionHandler, public timer::TimerTask
    {
        friend class Psub;

    public:
        Impl()
        {
            clear_mask();
        }

        Impl(const Impl &) = delete;
        Impl &operator=(const Impl &) = delete;
        ~Impl() override = default;

    public:
        void on_connected(net::Connection *conn) override;

        void on_error(net::Connection *conn) override
        {
            conn_ = nullptr;
            set_mask(RedisState::closed);
            completion_event_.notify();
        }

        void on_read(net::Connection *conn) override;

        void on_write(net::Connection *conn) override;

        void on_close(net::Connection *conn) override
        {
            conn_ = nullptr;
            set_mask(RedisState::closed, true);
            completion_event_.notify();
        }

    public:
        void on_timer(timer::Timer *timer) override;

    public:
        void on_do_connect(net::Connection *conn);

        void close();

        void set_mask(RedisState state, const bool only = false)
        {
            if (only)
                clear_mask();
            mask_ |= 1 << static_cast<uint8_t>(state);
        }

        void clear_mask(RedisState state)
        {
            mask_ &= ~(1 << static_cast<uint8_t>(state));
        }

        void clear_mask()
        {
            mask_ = 0;
        }

        bool is_connecting() const
        {
            return mask_ & 1 << static_cast<uint8_t>(RedisState::connecting);
        }

        bool is_connected() const
        {
            return mask_ & 1 << static_cast<uint8_t>(RedisState::connected);
        }

        bool is_closed() const
        {
            return mask_ & 1 << static_cast<uint8_t>(RedisState::closed);
        }

        bool is_disconnecting() const
        {
            return mask_ & 1 << static_cast<uint8_t>(RedisState::disconnecting);
        }

        bool is_timeout() const
        {
            return mask_ & 1 << static_cast<uint8_t>(RedisState::timeout);
        }

        void check_timeout();

        std::shared_ptr<RedisValue> execute_command(std::shared_ptr<Command> cmd);

        buffer::ByteBufferReader *get_reader()
        {
            return &reader_;
        }

        int fetch_next_message(int timeout);

    public:
        uint8_t mask_ = 0;
        RedisClient *client_ = nullptr;
        Option option_;
        net::Connection *conn_ = nullptr;
        std::shared_ptr<Command> last_cmd_;
        std::shared_ptr<MultiCmd> multi_cmd_;
        std::shared_ptr<SubcribeCmd> subcribe_cmd;
        std::shared_ptr<RedisValue> last_error_;
        time_t last_check_time_ = 0;
        buffer::ByteBufferReader reader_;
        yuan::coroutine::CompletionEvent completion_event_;
    };
}

#endif // __YUAN_REDIS_CLIENT_INTERNAL_IMPL_H__
