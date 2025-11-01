#include "redis_client.h"
#include "event/event_loop.h"
#include "internal/coroutine.h"
#include "internal/redis_registry.h"
#include "net/socket/socket.h"
#include "net/connection/tcp_connection.h"
#include "net/socket/inet_address.h"
#include "redis_value.h"
#include "internal/redis_impl.h"
#include "value/error_value.h"

#include <memory>

namespace yuan::redis
{
    RedisClient::RedisClient(const Option &opt)
    {
        impl_ = std::make_unique<Impl>();
        impl_->option_ = opt;
        impl_->client_ = this;
    }
    
    RedisClient::~RedisClient()
    {
        disconnect();
    }

    static SimpleTask<bool> do_connect(RedisClient *client)
    {
        co_await std::suspend_always{};
        RedisRegistry::get_instance()->get_event_loop()->loop();
        co_return client->is_connected();
    }

    int RedisClient::connect()
    {
        using namespace yuan::net;
        impl_->clear_mask();

        InetAddress addr{ impl_->option_.host_.empty() ? "127.0.0.1" : impl_->option_.host_.c_str(), impl_->option_.port_ };
        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            impl_->last_error_ = ErrorValue::from_string("port is invalid");
            return -1;
        }
        
        net::Socket *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            delete sock;
            sock = nullptr;
            impl_->last_error_ = ErrorValue::from_string("create socket failed");
            return -1;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            delete sock;
            sock = nullptr;
            impl_->last_error_ = ErrorValue::from_string("connect failed");
            return -1;
        }

        auto loop = RedisRegistry::get_instance()->get_event_loop();
        
        Connection *conn = new TcpConnection(sock);
        conn->set_connection_handler(impl_.get());
        conn->set_event_handler(loop);
        loop->update_channel(conn->get_channel());

        impl_->set_mask(RedisState::connecting);

        auto co = do_connect(this);
        bool res = co.execute();

        return res ? 0 : -1;
    }

    void RedisClient::set_option(const Option &opt)
    {
        impl_->option_ = opt;
    }

    bool RedisClient::is_connected() const
    {
        return impl_->is_connected();
    }

    void RedisClient::disconnect()
    {
        impl_->last_error_ = nullptr;
        if (!is_connected()) {
            return;
        }

        impl_->clear_mask();
        impl_->set_mask(RedisState::closed);
        impl_->conn_->close();
        RedisRegistry::get_instance()->get_event_loop()->quit();
    }

    std::shared_ptr<RedisValue> RedisClient::get_last_error() const
    {
        return impl_->last_error_;
    }
}
