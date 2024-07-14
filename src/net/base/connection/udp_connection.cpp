#include "buffer/buffer.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/inet_address.h"
#include "net/base/acceptor/udp/udp_instance.h"
#include "net/base/acceptor/udp/adapter.h"
#include <cassert>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "net/base/connection/udp_connection.h"

namespace net
{
    UdpConnection::UdpConnection(const InetAddress &addr) : address_(std::move(addr))
    {
        active_ = true;
        closed_ = false;
        state_ = ConnectionState::closed;
        connectionHandler_ = nullptr;
        eventHandler_ = nullptr;
        instance_ = nullptr;
        alive_timer_ = nullptr;
        adapter_ = nullptr;
        idle_cnt_ = 0;
    }

    UdpConnection::UdpConnection(const InetAddress &addr, UdpAdapter *adapter) : UdpConnection(addr)
    {
        adapter_ = adapter;
    }

    UdpConnection::~UdpConnection()
    {
        closed_ = true;
        active_ = false;
        state_ = ConnectionState::closed;
        if (alive_timer_) {
            alive_timer_->cancel();
            alive_timer_ = nullptr;
        }

        if (adapter_) {
            adapter_->on_release();
            adapter_ = nullptr;
        }

        if (instance_) {
            instance_->on_connection_close(this);
        }
    }

    ConnectionState UdpConnection::get_connection_state()
    {
        return state_;
    }

    bool UdpConnection::is_connected()
    {
        return state_ == ConnectionState::connected;
    }

    const InetAddress & UdpConnection::get_remote_address()
    {
        return address_;
    }

    Buffer * UdpConnection::get_input_buff(bool take)
    {
        return take ? input_buffer_.take_current_buffer() : input_buffer_.get_current_buffer();
    }

    Buffer * UdpConnection::get_output_buff(bool take)
    {
        return take ? output_buffer_.take_current_buffer() : output_buffer_.get_current_buffer();
    }

    void UdpConnection::write(Buffer * buff)
    {
        if (!buff || closed_) {
            return;
        }

        output_buffer_.append_buffer(buff);
        if (output_buffer_.get_current_buffer()->empty()) {
            output_buffer_.get_current_buffer()->reset();
            output_buffer_.free_current_buffer();
        }

        if (adapter_) {
            int ret = adapter_->on_write();
            if (ret < 0) {
                connectionHandler_->on_error(this);
                do_close();
            } else {
                output_buffer_.get_current_buffer()->reset();
                if (output_buffer_.get_size() > 1) {
                    output_buffer_.free_current_buffer();
                }
            }
        }
        active_ = true;
    }

    void UdpConnection::write_and_flush(Buffer *buff)
    {
        if (!buff || closed_) {
            return;
        }

        write(buff);
        send();
    }

    void UdpConnection::send()
    {
        assert(state_ == ConnectionState::connected && connectionHandler_);
        while (output_buffer_.get_size() > 0) {
            auto buff = output_buffer_.get_current_buffer();
            if (!buff->empty()) {
                instance_->on_send(this, output_buffer_.take_current_buffer());
            }

            if (output_buffer_.get_size() == 1) {
                break;
            }
        }
    }

    // 丢弃所有未发送的数据
    void UdpConnection::abort()
    {
        closed_ = true;
        state_ = ConnectionState::closed;
        do_close();
    }

    // 发送完数据后返回
    void UdpConnection::close()
    {
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        closed_ = true;
        if (lastState == ConnectionState::connecting || output_buffer_.get_current_buffer()->readable_bytes() > 0) {
            send();
            return;
        }
        do_close();
    }

    ConnectionType UdpConnection::get_conn_type()
    {
        return ConnectionType::UDP;
    }

    Channel * UdpConnection::get_channel()
    {
        return nullptr;
    }

    void UdpConnection::set_connection_handler(ConnectionHandler *handler)
    {
        this->connectionHandler_ = handler;
    }

    void UdpConnection::on_read_event()
    {
        assert(state_ == ConnectionState::connected && connectionHandler_);
        LinkedBuffer *list = instance_->get_input_buff_list();
        input_buffer_.free_current_buffer(list->take_current_buffer());
        if (adapter_) {
            adapter_->on_recv();
        }
        active_ = true;
        connectionHandler_->on_read(this);
    }

    void UdpConnection::on_write_event()
    {
        connectionHandler_->on_write(this);
        if (output_buffer_.get_current_buffer()->readable_bytes() > 0) {
            send();
        }
    }

    void UdpConnection::set_event_handler(EventHandler *eventHandler)
    {
        eventHandler_ = eventHandler;
    }

    void UdpConnection::do_close()
    {
        delete this;
    }

    ConnectionHandler * UdpConnection::get_connection_handler()
    {
        return connectionHandler_;
    }

    void UdpConnection::set_instance_handler(UdpInstance *instance)
    {
        instance_ = instance;
    }

    void UdpConnection::set_connection_state(ConnectionState state)
    {
        if (state_ == ConnectionState::connected) {
            return;
        }

        state_ = state;
        if (state_ == ConnectionState::connected) {
            connectionHandler_->on_connected(this);
            alive_timer_ = instance_->get_timer_manager()->interval(0, 10 * 1000, this, -1);
        }
    }

    void UdpConnection::on_timer(timer::Timer *timer)
    {
        if (!active_) {
            do_close();
            return;
        }

        if (output_buffer_.get_current_buffer()->empty()) {
            ++idle_cnt_;
        }

        // 第三次释放
        if (idle_cnt_ >= 2) {
            active_ = false;
        }
    }
}