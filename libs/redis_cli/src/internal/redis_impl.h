#include "cmd/multi_cmd.h"
#include "cmd/subcribe_cmd.h"
#include "internal/coroutine.h"
#include "option.h"
#include "redis_client.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include <cstdint>
#include <memory>

namespace yuan::redis 
{
    enum class RedisState : uint8_t
    {
        connecting = 1,
        connected = 2,
        disconnecting = 3,
        closed = 4,
    };

    class RedisClient::Impl : public net::ConnectionHandler
    {
    public:
        Impl() = default;
        ~Impl() = default;

    public:
        virtual void on_connected(net::Connection *conn);

        virtual void on_error(net::Connection *conn)
        {
            conn_ = nullptr;
            set_mask(RedisState::closed);
        }

        virtual void on_read(net::Connection *conn);

        virtual void on_write(net::Connection *conn);

        virtual void on_close(net::Connection *conn)
        {
            conn_ = nullptr;
            set_mask(RedisState::closed);
        }

        void close();

        void set_mask(RedisState state)
        {
            mask_ |= (1 << (uint8_t)state);
        }

        void clear_mask()
        {
            mask_ = 0;
        }
        
        bool is_connecting() const
        {
            return mask_ & (1 << (uint8_t)RedisState::connecting);
        }

        bool is_connected() const
        {
            return mask_ & (1 << (uint8_t)RedisState::connected);
        }

        bool is_closed() const
        {
            return mask_ & (1 << (uint8_t)RedisState::closed);
        }

        bool is_disconnecting() const
        {
            return mask_ & (1 << (uint8_t)RedisState::disconnecting);
        }

        std::shared_ptr<RedisValue> execute_command(std::shared_ptr<Command> cmd);

    public:
        uint8_t mask_ = 0;
        RedisClient *client_ = nullptr;
        Option option_;
        net::Connection *conn_;
        std::shared_ptr<Command> last_cmd_;
        std::shared_ptr<MultiCmd> multi_cmd_;
        std::shared_ptr<SubcribeCmd> subcribe_cmd_;
        std::shared_ptr<RedisValue> last_error_;
    };

    SimpleTask<std::shared_ptr<RedisValue>> do_execute_command(std::shared_ptr<Command> cmd);
}