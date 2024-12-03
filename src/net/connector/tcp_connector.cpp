#include "net/connector/tcp_connector.h"
#include "net/connection/tcp_connection.h"
#include "net/handler/event_handler.h"
#include "net/socket/socket.h"
#include "timer/timer_manager.h"
#include "net/handler/connector_handler.h"
#include "timer/timer_util.hpp"

#include <cassert>
#include <memory>

namespace net 
{
    class TcpConnector::TcpConnectorData
    {
    public:
        ~TcpConnectorData()
        {
            if (conn_timer_) {
                conn_timer_->cancel();
                conn_timer_ = nullptr;
            }
        }

    public:
        int timeout_ = 10 * 1000;
        int retry_count_ = 0;
        InetAddress address_;
        timer::TimerManager *timer_manager_ = nullptr;
        EventHandler *event_handler_ = nullptr;
        ConnectorHandler *connector_handler_ = nullptr;
        std::shared_ptr<SSLModule> ssl_module = nullptr;
        Connection *conn_ = nullptr;
        Socket *sock_ = nullptr;
        timer::Timer *conn_timer_ = nullptr;
    };

    TcpConnector::TcpConnector() : data_(std::make_unique<TcpConnector::TcpConnectorData>())
    {
    }

    TcpConnector::~TcpConnector() {}

    void TcpConnector::on_connected(Connection *conn)
    {
        if (data_->ssl_module) {
            auto sslHandler = data_->ssl_module->create_handler(conn->get_channel()->get_fd(), SSLHandler::SSLMode::connector_);
            if (!sslHandler) {
                data_->connector_handler_->on_connect_failed(conn);
                return;
            }

            if (!sslHandler->ssl_init_action()) {
                data_->connector_handler_->on_connect_failed(conn);
                return;
            }

            conn->set_ssl_handler(sslHandler);
        }

        if (data_->conn_timer_) {
            data_->conn_timer_->cancel();
            data_->conn_timer_ = nullptr;
        }

        data_->connector_handler_->on_connected_success(conn);
    }

    void TcpConnector::on_error(Connection *conn) {}

    void TcpConnector::on_read(Connection *conn) {}

    void TcpConnector::on_write(Connection *conn) {}

    void TcpConnector::on_close(Connection *conn) 
    {
        data_->connector_handler_->on_connect_failed(conn);
    }

    bool TcpConnector::connect(const InetAddress &address, int timeout /*= 10 * 1000*/, int retryCount /*= 3*/)
    {
        assert(data_->connector_handler_ && data_->timer_manager_ && data_->event_handler_);

        data_->address_ = address;
        data_->timeout_ = timeout;
        data_->retry_count_ = retryCount;

        Socket *sock = new Socket(address.get_ip().c_str(), address.get_port());
        sock->set_none_block(true);
        if (!sock->valid()) {
            delete sock;
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            delete sock;
            return false;
        }

        Connection *conn = new TcpConnection(sock);
        conn->set_connection_handler(this);
        conn->set_event_handler(data_->event_handler_);
        data_->conn_ = conn;
        data_->sock_ = sock;

        data_->conn_timer_ = timer::TimerUtil::build_timeout_timer(data_->timer_manager_, data_->timeout_, this, &TcpConnector::on_connect_timeout);

        return true;
    }

    void TcpConnector::set_data(timer::TimerManager *timerManager, ConnectorHandler *connectorHandler, EventHandler *eventHander)
    {
        data_->timer_manager_ = timerManager;
        data_->connector_handler_ = connectorHandler;
        data_->event_handler_ = eventHander;
    }

    void TcpConnector::set_ssl_module(std::shared_ptr<SSLModule> module)
    {
        data_->ssl_module = module;
    }

    int TcpConnector::get_retry_count()
    {
        return data_->retry_count_;
    }

    void TcpConnector::cancel()
    {
        if (data_->conn_timer_) {
            data_->conn_timer_->cancel();
            data_->conn_timer_ = nullptr;
        }
    }

    void TcpConnector::on_connect_timeout(timer::Timer *timer)
    {
        data_->conn_timer_->cancel();
        if (data_->retry_count_ > 0) {
            --data_->retry_count_;
            if (!data_->sock_->connect()) {
                data_->connector_handler_->on_connect_failed(data_->conn_);
                return;
            }
            data_->conn_timer_ = timer::TimerUtil::build_timeout_timer(data_->timer_manager_, data_->timeout_, this, &TcpConnector::on_connect_timeout);
        } else {
            data_->connector_handler_->on_connect_timeout(data_->conn_);
        }
    }
}