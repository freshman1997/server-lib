#include "redis_client.h"
#include "coroutine/sync_wait.h"
#include "event/event_loop.h"
#include "internal/coroutine.h"
#include "internal/redis_registry.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/socket/socket.h"
#include "net/socket/inet_address.h"
#include "redis_value.h"
#include "internal/redis_impl.h"
#include "value/error_value.h"

#include <memory>

namespace yuan::redis
{
    RedisClient::RedisClient(const Option & opt)
    {
        impl_ = std::make_unique<Impl>();
        impl_->option_ = opt;
        impl_->client_ = this;
    }

    RedisClient::~RedisClient()
    {
        close();
    }

    int RedisClient::connect()
    {
        using namespace yuan::net;

        const InetAddress addr{ impl_->option_.host_, impl_->option_.port_ };
        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            impl_->last_error_ = ErrorValue::from_string("port is invalid");
            return -1;
        }

        auto *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            delete sock;
            sock = nullptr;
            impl_->last_error_ = ErrorValue::from_string("create socket failed");
            return -1;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            const int socket_error = sock->last_error();
            delete sock;
            sock = nullptr;
            impl_->last_error_ = ErrorValue::from_string(
                impl_->option_.name_ + " connect failed, socket error: " + std::to_string(socket_error));
            return -1;
        }

        const auto loop = RedisRegistry::get_instance()->get_event_loop();

        Connection *conn = create_stream_connection(sock);
        conn->set_connection_handler(impl_.get());
        conn->set_event_handler(loop);
        if (auto *stream = dynamic_cast<net::StreamTransport *>(conn)) {
            auto *channel = stream->stream_channel();
            if (!channel) {
                conn->close();
                impl_->last_error_ = ErrorValue::from_string("stream channel is invalid");
                return -1;
            }
            loop->update_channel(channel);
        } else {
            conn->close();
            impl_->last_error_ = ErrorValue::from_string("connection is not a stream transport");
            return -1;
        }

        impl_->on_do_connect(conn);
        impl_->completion_event_.reset(loop);

        const auto runtime = RedisRegistry::get_instance()->get_coroutine_runtime();
        auto wait_connect = [this]()->SimpleTask<bool>
        {
            const auto timer_manager = RedisRegistry::get_instance()->get_timer_manager();
            const bool timed_out = co_await impl_->completion_event_.wait_for(
                timer_manager,
                impl_->option_.timeout_ms_ > 0 ? static_cast<uint32_t>(impl_->option_.timeout_ms_) : 0);
            co_return !timed_out && is_connected();
        };
        const bool res = yuan::coroutine::sync_wait(runtime, wait_connect());

        if (!res) {
            impl_->last_error_ = ErrorValue::from_string(impl_->option_.name_ + " connect timeout");
            impl_->close();
            return -1;
        }

        impl_->clear_mask(RedisState::connecting);

        if (!impl_->option_.password_.empty()) {
            if (const auto auth_result = auth(impl_->option_.password_); !auth_result) {
                impl_->last_error_ = ErrorValue::from_string("auth failed");
                close();
                return -1;
            }
        }

        if (impl_->option_.db_ != 0) {
            if (const auto select_result = select(impl_->option_.db_); !select_result) {
                impl_->last_error_ = ErrorValue::from_string("select db failed");
                close();
                return -1;
            }
        }

        return 0;
    }

    void RedisClient::set_option(const Option & opt)
    {
        impl_->option_ = opt;
    }

    bool RedisClient::is_connected() const
    {
        return impl_->is_connected();
    }

    bool RedisClient::is_closed() const
    {
        return impl_->is_closed();
    }

    bool RedisClient::is_timeout() const
    {
        return impl_->is_timeout();
    }

    void RedisClient::close()
    {
        if (!is_connected()) {
            return;
        }

        impl_->set_mask(RedisState::closed);
        impl_->close();
    }

    std::shared_ptr<RedisValue> RedisClient::get_last_error() const
    {
        return impl_->last_error_;
    }

    void RedisClient::set_last_error(std::shared_ptr<RedisValue> error)
    {
        impl_->last_error_ = error;
    }

    const std::string &RedisClient::get_name() const
    {
        return impl_->option_.name_;
    }

    void RedisClient::unsubscribe_channel(const std::string & channel)
    {
        if (impl_->subcribe_cmd) {
            impl_->subcribe_cmd->unsubcribe(channel);
            if (!impl_->subcribe_cmd->is_subcribe()) {
                impl_->subcribe_cmd = nullptr;
            }
        }
    }

    int RedisClient::receive(int timeout)
    {
        return impl_->fetch_next_message(timeout);
    }

    int RedisClient::receive()
    {
        return impl_->fetch_next_message(0);
    }

    bool RedisClient::is_subscribing() const
    {
        return impl_->subcribe_cmd != nullptr && impl_->subcribe_cmd->is_subcribe();
    }
}
