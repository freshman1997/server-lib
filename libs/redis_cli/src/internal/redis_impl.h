#include "cmd/multi_cmd.h"
#include "option.h"
#include "redis_client.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include <memory>

namespace yuan::redis 
{
    class RedisClient::Impl : public net::ConnectionHandler
    {
    public:
        Impl() = default;
        ~Impl() = default;

    public:
        virtual void on_connected(net::Connection *conn)
        {
        }

        virtual void on_error(net::Connection *conn)
        {
        }

        virtual void on_read(net::Connection *conn);

        virtual void on_write(net::Connection *conn);

        virtual void on_close(net::Connection *conn)
        {

        }

    public:
        std::shared_ptr<MultiCmd> multi_cmd_;
        Option option_;
        std::shared_ptr<Command> last_cmd_;
        std::shared_ptr<RedisValue> last_error_;
    };
}