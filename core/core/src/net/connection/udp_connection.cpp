#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/handler/event_handler.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/acceptor/udp/adapter.h"
#include <cassert>
#include <cerrno>
#ifndef _WIN32
#include <unistd.h>
#else
#include <winsock2.h>
#endif

#include "net/connection/udp_connection.h"

namespace yuan::net
{
    UdpConnection::UdpConnection(const InetAddress &addr) : address_(std::move(addr)), Connection()
    {
        active_ = true;
        closed_ = false;
        is_closing_ = false;
        state_ = ConnectionState::connecting;
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

        // Guard against use-after-free: if UdpInstance is being destroyed,
        // skip on_connection_close to avoid accessing a destroyed object
        if (instance_ && !instance_->is_closing()) {
            instance_->on_connection_close(this);
            instance_ = nullptr;
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
                abort();  // 改用 abort() 以使用延迟删除
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

        // Adapter mode: flush pending encoded data first
        if (adapter_) {
            process_pending_output_buffer();
            if (is_closing_) return;
        }

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
                        instance_->enable_rw_events();
                        return;
                    }
                    ++i;
                } else if (sent < 0) {
#ifdef _WIN32
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS) {
#endif
                        abort();
                        return;
                    }
                } else {
                    instance_->enable_rw_events();
                    break;
                }
            }
        }

        if (output_buffer_.get_current_buffer()->empty() && closed_ && !is_closing_) {
            is_closing_ = true;
            // 使用延迟删除，避免在事件回调中直接删除
            if (eventHandler_) {
                eventHandler_->queue_in_loop([this]() {
                    delete this;
                });
            } else {
                delete this;
            }
        }
    }

    // 丢弃所有未发送的数据，立即标记销毁
    void UdpConnection::abort()
    {
        if (is_closing_) {
            return;  // 防止重复销毁
        }
        is_closing_ = true;
        closed_ = true;
        state_ = ConnectionState::closed;
        // 使用延迟删除，避免在事件回调中直接删除
        if (eventHandler_) {
            eventHandler_->queue_in_loop([this]() {
                delete this;
            });
        } else {
            delete this;
        }
    }

    // 发送完数据后返回
    void UdpConnection::close()
    {
        if (is_closing_ || closed_) {
            return;  // 防止重复调用
        }
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        closed_ = true;
        if (lastState == ConnectionState::connecting || output_buffer_.get_current_buffer()->readable_bytes() > 0) {
            flush();
            return;
        }
        is_closing_ = true;
        // 使用延迟删除，避免在事件回调中直接删除
        if (eventHandler_) {
            eventHandler_->queue_in_loop([this]() {
                delete this;
            });
        } else {
            delete this;
        }
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
        if (is_closing_) {
            return;
        }
        is_closing_ = true;
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
        if (is_closing_) {
            return;  // 已在销毁过程中，忽略 timer 回调
        }

        if (!active_) {
            abort();
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
                abort();  // 改用 abort() 以使用延迟删除，避免 use-after-free
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