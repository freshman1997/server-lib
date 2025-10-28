#include "redis_client.h"
#include "event/event_loop.h"
#include "internal/def.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "option.h"
#include "internal/command_manager.h"
#include "internal/redis_registry.h"
#include "net/socket/socket.h"
#include "net/connection/tcp_connection.h"
#include "net/socket/inet_address.h"
#include "value/error_value.h"

#include <iostream>

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
            //conn->get_output_buff()->write_string("PING\r\n");
            //conn->flush();
        }

        virtual void on_error(net::Connection *conn)
        {
        }

        virtual void on_read(net::Connection *conn)
        {
            if (!last_cmd_)
            {
                return;
            }

            auto buff = conn->get_input_buff();
            int ret = last_cmd_->unpack((const unsigned char *)buff->peek(), (const unsigned char *)buff->peek_end());
            if (ret < 0)
            {
                if (*buff->peek() == resp_error)
                {
                    last_error_ = std::make_shared<ErrorValue>(std::string((const char *)buff->peek() + 1, buff->readable_bytes() - 3));
                }
            }

            last_cmd_->on_executed();
            last_cmd_ = nullptr;
        }

        virtual void on_write(net::Connection *conn)
        {
            if (last_cmd_)
            {
                return;
            }

            last_error_ = nullptr;
            last_cmd_ = CommandManager::get_instance()->get_command();
            if (last_cmd_)
            {
                const auto& cmdStr = last_cmd_->pack();
                conn->get_output_buff()->write_string(cmdStr);
            }
        }

        virtual void on_close(net::Connection *conn)
        {

        }

    public:
        Option option_;
        std::shared_ptr<Command> last_cmd_;
        std::shared_ptr<RedisValue> last_error_;
    };

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

    int RedisClient::execute_command(std::shared_ptr<Command> cmd)
    {
        CommandManager::get_instance()->add_command(cmd);

        return 0;
    }

    std::shared_ptr<RedisValue> RedisClient::get_last_error() const
    {
        return impl_->last_error_;
    }
}
