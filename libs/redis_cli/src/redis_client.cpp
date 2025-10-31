#include "redis_client.h"
#include "event/event_loop.h"
#include "internal/coroutine.h"
#include "net/connection/connection.h"
#include "option.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "net/socket/socket.h"
#include "net/connection/tcp_connection.h"
#include "net/socket/inet_address.h"
#include "redis_value.h"
#include "internal/redis_impl.h"

#include <iostream>
#include <memory>

namespace yuan::redis
{
    RedisClient::RedisClient()
    {
        impl_ = std::make_unique<Impl>();
    }
    
    RedisClient::~RedisClient()
    {
    }

    int RedisClient::connect(const std::string &host, int port)
    {
        if (port <= 0 || port > 65535)
        {
            return -1;
        }

        if (host.empty())
        {
            return -1;
        }

        impl_->option_.host_ = host;
        impl_->option_.port_ = port;
        
        using namespace yuan::net;

        InetAddress addr{ impl_->option_.host_.c_str(), impl_->option_.port_ };
        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            std::cout << "port is invalid!!\n";
            return false;
        }
        
        net::Socket *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cout << " connect failed " << std::endl;
            return false;
        }

        Connection *conn = new TcpConnection(sock);

        auto loop = RedisRegistry::get_instance()->get_event_loop();
        
        conn->set_connection_handler(impl_.get());
        conn->set_event_handler(loop);
        loop->update_channel(conn->get_channel());

        return 0;
    }

    int RedisClient::connect(const std::string &host, int port, const std::string &password)
    {
        if (password.empty())
        {
            return connect(host, port);
        }
        
        impl_->option_.password_ = password;

        return connect(host, port);
    }

    int RedisClient::connect(const std::string &host, int port, const std::string &password, int db)
    {
        if (db < 0)
        {
            return -1;
        }

        impl_->option_.db_ = db;

        return connect(host, port, password);
    }

    int RedisClient::connect(const std::string &host, int port, const std::string &password, int db, int timeout)
    {
        if (timeout <= 0)
        {
            return -1;
        }

        impl_->option_.timeout_ms_ = timeout;

        return connect(host, port, password, db);
    }

    SimpleTask<std::shared_ptr<RedisValue>> do_execute_command(std::shared_ptr<Command> cmd)
    {
        co_await std::suspend_always{};
        RedisRegistry::get_instance()->get_event_loop()->loop();
        co_return cmd->get_result();
    }

    SimpleTask<std::shared_ptr<RedisValue>> RedisClient::execute_command(std::shared_ptr<Command> cmd)
    {
        if (impl_->multi_cmd_)
        {
            impl_->multi_cmd_->add_command(cmd);
            co_return nullptr;
        }

        impl_->last_error_ = nullptr;
        CommandManager::get_instance()->add_command(cmd);
        auto co = do_execute_command(cmd);
        co_return co.execute();
    }

    std::shared_ptr<RedisValue> RedisClient::get_last_error() const
    {
        return impl_->last_error_;
    }
}
