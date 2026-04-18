#include "net/connector/tcp_connector.h"

#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/channel/channel.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connector_handler.h"
#include "net/handler/event_handler.h"
#include "event/event_loop.h"
#include "net/secuity/ssl_handler.h"
#include "net/secuity/ssl_module.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

#include <cassert>
#include <memory>

namespace yuan::net
{
    class TcpConnector::TcpConnectorData
    {
    public:
        ~TcpConnectorData()
        {
            cancel_timer();
        }

    public:
        void cancel_timer()
        {
            if (conn_timer_) {
                conn_timer_->cancel();
                conn_timer_ = nullptr;
            }
        }

        void reset_connection_state()
        {
            conn_ = nullptr;
        }

        int timeout_ = 10 * 1000;
        int retry_count_ = 0;
        InetAddress address_;
        timer::TimerManager *timer_manager_ = nullptr;
        EventHandler *event_handler_ = nullptr;
        ConnectorHandler *connector_handler_ = nullptr;
        std::shared_ptr<SSLModule> ssl_module = nullptr;
        ConnectionPtr conn_;
        timer::Timer *conn_timer_ = nullptr;
        bool suppress_failure_callback_ = false;
        bool connected_ = false;
    };

    TcpConnector::TcpConnector()
        : data_(std::make_unique<TcpConnectorData>())
    {
    }

    TcpConnector::~TcpConnector() = default;

    void TcpConnector::on_connected(const std::shared_ptr<Connection> & conn)
    {
        if (!conn) {
            return;
        }

        if (data_->ssl_module) {
            auto stream = std::dynamic_pointer_cast<StreamTransport>(conn);
            auto *channel = stream ? stream->stream_channel() : nullptr;
            if (!channel) {
                data_->reset_connection_state();
                data_->connector_handler_->on_connect_failed(conn);
                return;
            }

            const auto sslHandler = data_->ssl_module->create_handler(channel->get_fd(), SSLHandler::SSLMode::connector_);
            if (!sslHandler) {
                data_->reset_connection_state();
                data_->connector_handler_->on_connect_failed(conn);
                return;
            }

            conn->set_ssl_handler(sslHandler);
            conn->set_ssl_handshaking(true);

            int ret = sslHandler->ssl_init_action();
            if (ret > 0) {
                conn->set_ssl_handshaking(false);
            } else if (sslHandler->ssl_want_read() || sslHandler->ssl_want_write()) {
                if (sslHandler->ssl_want_write()) {
                    channel->enable_write();
                    if (conn->get_connection_handler() && data_->event_handler_) {
                        data_->event_handler_->update_channel(channel);
                    }
                }
                conn->set_ssl_handshake_callback([this, conn](bool success) {
                    if (success) {
                        data_->cancel_timer();
                        data_->connected_ = true;
                        data_->connector_handler_->on_connected_success(conn);
                    } else {
                        data_->reset_connection_state();
                        data_->connector_handler_->on_connect_failed(conn);
                    }
                });
                return;
            } else {
                conn->set_ssl_handshaking(false);
                data_->reset_connection_state();
                data_->connector_handler_->on_connect_failed(conn);
                return;
            }
        }

        data_->cancel_timer();
        data_->connected_ = true;
        data_->connector_handler_->on_connected_success(conn);
    }

    void TcpConnector::on_error(const std::shared_ptr<Connection> & conn)
    {
        (void)conn;
        data_->reset_connection_state();
    }

    void TcpConnector::on_read(const std::shared_ptr<Connection> & conn)
    {
        (void)conn;
    }

    void TcpConnector::on_write(const std::shared_ptr<Connection> & conn)
    {
        (void)conn;
    }

    void TcpConnector::on_close(const std::shared_ptr<Connection> & conn)
    {
        data_->reset_connection_state();
        if (data_->suppress_failure_callback_) {
            data_->suppress_failure_callback_ = false;
            return;
        }

        if (data_->connected_) {
            data_->connected_ = false;
            return;
        }

        data_->connector_handler_->on_connect_failed(conn);
    }

    bool TcpConnector::connect(const InetAddress & address, int timeout /*= 10 * 1000*/, int retryCount /*= 3*/)
    {
        assert(data_->connector_handler_ && data_->timer_manager_ && data_->event_handler_);

        data_->address_ = address;
        data_->timeout_ = timeout;
        data_->retry_count_ = retryCount;
        data_->cancel_timer();
        data_->reset_connection_state();
        data_->connected_ = false;
        data_->suppress_failure_callback_ = false;

        auto sock = std::make_unique<Socket>(data_->address_.get_ip(), data_->address_.get_port());
        sock->set_none_block(true);
        if (!sock->valid()) {
            return false;
        }

        if (!sock->connect()) {
            return false;
        }

        auto conn = create_stream_connection(sock.release());
        conn->set_connection_handler(make_non_owning_handler(this));
        conn->set_event_handler(data_->event_handler_);
        if (auto *loop = dynamic_cast<EventLoop *>(data_->event_handler_)) {
            loop->on_new_connection(conn);
        } else if (data_->event_handler_) {
            data_->event_handler_->on_new_connection(conn);
        }
        data_->conn_ = conn;
        data_->conn_timer_ = timer::TimerUtil::build_timeout_timer(data_->timer_manager_, data_->timeout_, this, &TcpConnector::on_connect_timeout);

        return true;
    }

    void TcpConnector::set_data(timer::TimerManager * timerManager, ConnectorHandler * connectorHandler, EventHandler * eventHander)
    {
        data_->timer_manager_ = timerManager;
        data_->connector_handler_ = connectorHandler;
        data_->event_handler_ = eventHander;
    }

    void TcpConnector::set_ssl_module(std::shared_ptr<SSLModule> module)
    {
        data_->ssl_module = module;
    }

    int TcpConnector::get_retry_count() const
    {
        return data_->retry_count_;
    }

    void TcpConnector::cancel()
    {
        data_->cancel_timer();
        if (data_->conn_) {
            data_->suppress_failure_callback_ = true;
            data_->conn_->close();
            data_->conn_ = nullptr;
        }
    }

    void TcpConnector::on_connect_timeout(timer::Timer * timer)
    {
        data_->cancel_timer();
        if (data_->retry_count_ > 0) {
            --data_->retry_count_;
            if (data_->conn_) {
                data_->suppress_failure_callback_ = true;
                data_->conn_->close();
                data_->conn_ = nullptr;
            }

            auto sock = std::make_unique<Socket>(data_->address_.get_ip(), data_->address_.get_port());
            sock->set_none_block(true);
            if (!sock->valid() || !sock->connect()) {
                data_->connector_handler_->on_connect_failed(data_->conn_);
                return;
            }

            auto conn = create_stream_connection(sock.release());
            conn->set_connection_handler(make_non_owning_handler(this));
            conn->set_event_handler(data_->event_handler_);
            if (auto *loop = dynamic_cast<EventLoop *>(data_->event_handler_)) {
                loop->on_new_connection(conn);
            } else if (data_->event_handler_) {
                data_->event_handler_->on_new_connection(conn);
            }
            data_->conn_ = conn;
            data_->connected_ = false;
            data_->suppress_failure_callback_ = false;
            data_->conn_timer_ = timer::TimerUtil::build_timeout_timer(data_->timer_manager_, data_->timeout_, this, &TcpConnector::on_connect_timeout);
        } else {
            auto timed_out_conn = data_->conn_;
            if (data_->conn_) {
                data_->suppress_failure_callback_ = true;
                data_->conn_ = nullptr;
                timed_out_conn->close();
            }
            data_->connector_handler_->on_connect_timeout(timed_out_conn);
        }
    }
}
