#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/acceptor/udp/adapter.h"
#include <cassert>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "net/connection/udp_connection.h"

namespace yuan::net
{
    UdpConnection::UdpConnection(const InetAddress &addr) : address_(std::move(addr)), Connection()
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

        if (connectionHandler_) {
            connectionHandler_->on_close(this);
            connectionHandler_ = nullptr;
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
    
    void UdpConnection::write(buffer::Buffer * buff)
    {
        if (!buff || closed_) {
            buffer::BufferedPool::get_instance()->free(buff);
            return;
        }

        if (adapter_) {
            if (!proc_one_buff(buff)) {
                connectionHandler_->on_error(this);
                do_close();
                return;
            }
        } else {
            output_buffer_.append_buffer(buff);
            if (output_buffer_.get_current_buffer()->empty()) {
                output_buffer_.get_current_buffer()->reset();
                output_buffer_.free_current_buffer();
            }
        }

        active_ = true;
    }

    void UdpConnection::write_and_flush(buffer::Buffer *buff)
    {
        if (!buff || closed_) {
            buffer::BufferedPool::get_instance()->free(buff);
            return;
        }

        write(buff);
        flush();
    }

    void UdpConnection::flush()
    {
        assert(connectionHandler_);
        if (!output_buffer_.get_current_buffer()->empty()) {
            std::size_t sz = output_buffer_.get_size();
            for (int i = 0; i < sz;) {
                auto buff = output_buffer_.get_current_buffer();
                int sent = instance_->on_send(this, buff);
                if (sent > 0) {
                    buff->add_read_index(sent);
                    if (buff->empty()) {
                        output_buffer_.free_current_buffer();
                    } else {
                        return;
                    }
                    ++i;
                } else if (sent < 0 && EAGAIN != errno) {
                    abort();
                    return;
                } else {
                    instance_->enable_rw_events();
                    break;
                }
            }
        }

        if (output_buffer_.get_current_buffer()->empty() && closed_) {
            closed_ = false;
            do_close();
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
            flush();
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
        assert(state_ == ConnectionState::connected && connectionHandler_ && input_buffer_);
        auto *list = instance_->get_input_buff_list();
        const auto& buffers = list->to_vector();

        for (auto buf : buffers) {
            input_buffer_->append_buffer(*buf);
        }

        list->free_all_buffers();

        bool ok = adapter_ ? adapter_->on_recv(input_buffer_) > 0 : true;
        if (ok) {
            active_ = true;
            connectionHandler_->on_read(this);
        } else {
            abort();
        }
    }

    void UdpConnection::on_write_event()
    {
        connectionHandler_->on_write(this);
        process_pending_output_buffer();
        if (!output_buffer_.get_current_buffer()->empty()) {
            flush();
        }
    }

    void UdpConnection::set_event_handler(EventHandler *eventHandler)
    {
        eventHandler_ = eventHandler;
    }

    void UdpConnection::do_close()
    {
        if (closed_) {
            return;
        }
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

    void UdpConnection::forward(Connection *conn)
    {
        conn->write(get_input_buff(true));
    }

    void UdpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        assert(false);
    }

    bool UdpConnection::proc_one_buff(buffer::Buffer *buff)
    {
        int ret = adapter_->on_write(buff);
        if (ret < 0) {
            return false;
        } else {
            if (ret < buff->readable_bytes()) {
                buff->add_read_index(ret);
                pending_output_buffer_.append_buffer(buff);
            } else {
                buffer::BufferedPool::get_instance()->free(buff);
            }
            return true;
        }
    }

    void UdpConnection::process_pending_output_buffer()
    {
        if (pending_output_buffer_.get_current_buffer()->empty()) {
            return;
        }

        std::size_t sz = pending_output_buffer_.get_size();
        for (int i = 0; i < sz; ++i) {
            auto buff = pending_output_buffer_.get_current_buffer();
            if (!proc_one_buff(buff)) {
                connectionHandler_->on_error(this);
                do_close();
                return;
            }

            if (buff->empty()) {
                pending_output_buffer_.free_current_buffer();
            } else {
                return;
            }
        }
    }
}