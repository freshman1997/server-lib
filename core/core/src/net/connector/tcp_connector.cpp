#include "net/connector/tcp_connector.h"

#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/channel/channel.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connector_handler.h"
#include "net/handler/event_handler.h"
#include "event/event_loop.h"
#include "net/security/ssl_handler.h"
#include "net/security/ssl_module.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/socket_ops.h"
#include "timer/timer_manager.h"
#include "timer/timer_handle.h"
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
            conn_timer_.cancel();
            conn_timer_.reset();
        }

        void reset_connection_state()
        {
            conn_ = nullptr;
        }

        int timeout_ = 10 * 1000;
        int retry_count_ = 0;
        uint64_t attempt_id_ = 0;
        int last_error_ = 0;
        InetAddress address_;
        timer::TimerManager *timer_manager_ = nullptr;
        EventHandler *event_handler_ = nullptr;
        std::shared_ptr<ConnectorHandler> connector_handler_owner_;
        std::shared_ptr<SSLModule> ssl_module = nullptr;
        ConnectionPtr conn_;
        std::shared_ptr<ConnectionHandler> connect_handler_;
        timer::TimerHandle conn_timer_;
        bool suppress_failure_callback_ = false;
        bool connected_ = false;
    };

    class TcpConnector::ConnectAttemptHandler : public ConnectionHandler
    {
    public:
        explicit ConnectAttemptHandler(const std::shared_ptr<TcpConnectorData> &data)
            : data_(data)
        {
        }

        void on_connected(const std::shared_ptr<Connection> &conn) override
        {
            auto data = data_.lock();
            if (!data || !conn || !data->connector_handler_owner_) {
                return;
            }

            if (data->ssl_module) {
                auto stream = std::dynamic_pointer_cast<StreamTransport>(conn);
                auto *channel = stream ? stream->stream_channel() : nullptr;
                if (!channel) {
                    data->reset_connection_state();
                    data->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, conn, data->last_error_, data->attempt_id_});
                    return;
                }

                const auto sslHandler = data->ssl_module->create_handler(channel->get_fd(), SSLHandler::SSLMode::connector_);
                if (!sslHandler) {
                    data->reset_connection_state();
                    data->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, conn, data->last_error_, data->attempt_id_});
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
                        if (conn->get_connection_handler() && data->event_handler_) {
                            data->event_handler_->update_channel(channel);
                        }
                    }
                    conn->set_ssl_handshake_callback([weak_data = data_, conn](bool success) {
                        auto locked = weak_data.lock();
                        if (!locked || !locked->connector_handler_owner_) {
                            return;
                        }
                        if (success) {
                            locked->cancel_timer();
                            locked->connected_ = true;
                            locked->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::success, conn, 0, locked->attempt_id_});
                        } else {
                            locked->reset_connection_state();
                            locked->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, conn, locked->last_error_, locked->attempt_id_});
                        }
                    });
                    return;
                } else {
                    conn->set_ssl_handshaking(false);
                    data->reset_connection_state();
                    data->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, conn, data->last_error_, data->attempt_id_});
                    return;
                }
            }

            data->cancel_timer();
            data->connected_ = true;
            data->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::success, conn, 0, data->attempt_id_});
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
            if (auto data = data_.lock()) {
                data->reset_connection_state();
            }
        }

        void on_read(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_write(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_close(const std::shared_ptr<Connection> &conn) override
        {
            auto data = data_.lock();
            if (!data || !data->connector_handler_owner_) {
                return;
            }

            data->reset_connection_state();
            if (data->suppress_failure_callback_) {
                data->suppress_failure_callback_ = false;
                return;
            }

            if (data->connected_) {
                data->connected_ = false;
                return;
            }

            data->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, conn, data->last_error_, data->attempt_id_});
        }

    private:
        std::weak_ptr<TcpConnectorData> data_;
    };

    TcpConnector::TcpConnector()
        : data_(std::make_shared<TcpConnectorData>())
    {
        data_->connect_handler_ = std::make_shared<ConnectAttemptHandler>(data_);
    }

    TcpConnector::~TcpConnector() = default;

    bool TcpConnector::connect(const InetAddress & address, int timeout /*= 10 * 1000*/, int retryCount /*= 3*/)
    {
        assert(data_->connector_handler_owner_ && data_->timer_manager_ && data_->event_handler_);

        data_->address_ = address;
        data_->timeout_ = timeout;
        data_->retry_count_ = retryCount;
        data_->attempt_id_ += 1;
        data_->last_error_ = 0;
        data_->cancel_timer();
        data_->reset_connection_state();
        data_->connected_ = false;
        data_->suppress_failure_callback_ = false;

        auto sock = std::make_unique<Socket>(data_->address_.get_ip(), data_->address_.get_port());
        sock->set_none_block(true);
        if (!sock->valid()) {
            data_->last_error_ = socket::get_last_error();
            return false;
        }

        if (!sock->connect()) {
            data_->last_error_ = sock->last_error();
            return false;
        }

        auto conn = create_stream_connection(sock.release());
        conn->set_connection_handler(data_->connect_handler_);
        conn->set_event_handler(data_->event_handler_);
        if (auto *loop = dynamic_cast<EventLoop *>(data_->event_handler_)) {
            loop->on_new_connection(conn);
        } else if (data_->event_handler_) {
            data_->event_handler_->on_new_connection(conn);
        }
        data_->conn_ = conn;
        data_->conn_timer_ = timer::TimerUtil::build_timeout_handle(data_->timer_manager_, data_->timeout_, [this]() { on_connect_timeout(); });

        return true;
    }

    void TcpConnector::set_data(timer::TimerManager * timerManager,
                                std::shared_ptr<ConnectorHandler> connectorHandler,
                                EventHandler * eventHander)
    {
        data_->timer_manager_ = timerManager;
        data_->connector_handler_owner_ = std::move(connectorHandler);
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

    void TcpConnector::on_connect_timeout()
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
                data_->last_error_ = sock ? sock->last_error() : socket::get_last_error();
                data_->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::failed, data_->conn_, data_->last_error_, data_->attempt_id_});
                return;
            }

            auto conn = create_stream_connection(sock.release());
            conn->set_connection_handler(data_->connect_handler_);
            conn->set_event_handler(data_->event_handler_);
            if (auto *loop = dynamic_cast<EventLoop *>(data_->event_handler_)) {
                loop->on_new_connection(conn);
            } else if (data_->event_handler_) {
                data_->event_handler_->on_new_connection(conn);
            }
            data_->conn_ = conn;
            data_->connected_ = false;
            data_->suppress_failure_callback_ = false;
            data_->conn_timer_ = timer::TimerUtil::build_timeout_handle(data_->timer_manager_, data_->timeout_, [this]() { on_connect_timeout(); });
        } else {
            auto timed_out_conn = data_->conn_;
            if (data_->conn_) {
                data_->suppress_failure_callback_ = true;
                data_->conn_ = nullptr;
                timed_out_conn->close();
            }
            data_->connector_handler_owner_->on_connect_result(ConnectResult{ConnectResultCode::timeout, timed_out_conn, data_->last_error_, data_->attempt_id_});
        }
    }
}
