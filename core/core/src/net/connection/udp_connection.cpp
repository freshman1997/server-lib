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
    UdpConnection::UdpConnection(const InetAddress & addr)
        : address_(std::move(addr)), Connection()
    {
        set_max_packet_size(UDP_DATA_LIMIT);
        active_ = true;
        closed_ = false;
        is_closing_ = false;
        state_ = ConnectionState::connecting;
        connectionHandler_ = nullptr;
        eventHandler_ = nullptr;
        instance_ = nullptr;
        alive_timer_ = nullptr;
        idle_cnt_ = 0;
    }

    UdpConnection::UdpConnection(const InetAddress & addr, UdpAdapter * adapter)
        : UdpConnection(addr)
    {
        adapter_.reset(adapter);
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
            adapter_.reset();
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

    ConnectionState UdpConnection::get_connection_state() const
    {
        return state_;
    }

    bool UdpConnection::is_connected() const
    {
        return state_ == ConnectionState::connected;
    }

    const InetAddress &UdpConnection::get_remote_address() const
    {
        return address_;
    }

    const InetAddress &UdpConnection::peer_address() const
    {
        return address_;
    }

    void UdpConnection::write(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty() || closed_) {
            return;
        }

        if (adapter_) {
            if (!proc_one_buffer(buffer)) {
                connectionHandler_->on_error(this);
                abort();
                return;
            }
        } else {
            auto chunk = std::make_unique< ::yuan::buffer::ByteBuffer>(buffer.copy_readable());
            if (!chunk->empty()) {
                output_buffer_.push_back(std::move(chunk));
            }
        }

        active_ = true;
    }

    void UdpConnection::write_and_flush(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty() || closed_) {
            return;
        }

        write(buffer);
        flush();
    }
    void UdpConnection::flush()
    {
        assert(connectionHandler_);

        // Adapter mode: flush pending encoded data first
        if (adapter_) {
            process_pending_output_buffer();
            if (is_closing_)
                return;
        }

        auto *front = output_buffer_.front();
        if (front && !front->empty()) {
            std::size_t sz = output_buffer_.size();
            for (int i = 0; i < sz;) {
                front = output_buffer_.front();
                if (!front || front->empty()) {
                    break;
                }
                const auto packet = front->copy_readable();
                int sent = instance_->on_send(this, packet);
                if (sent > 0) {
                    front->consume(static_cast<std::size_t>(sent));
                    if (front->empty()) {
                        output_buffer_.pop_front();
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

        front = output_buffer_.front();
        if ((!front || front->empty()) && closed_ && !is_closing_) {
            is_closing_ = true;
            // 使用延迟删除，避免在事件回调中直接删�?
            if (eventHandler_) {
                eventHandler_->queue_in_loop([this]() {
                    delete this;
                });
            } else {
                delete this;
            }
        }
    }

    // 丢弃所有未发送的数据，立即标记销�?
    void UdpConnection::abort()
    {
        if (is_closing_) {
            return; // 防止重复销�?
        }
        is_closing_ = true;
        closed_ = true;
        state_ = ConnectionState::closed;
        // 使用延迟删除，避免在事件回调中直接删�?
        if (eventHandler_) {
            eventHandler_->queue_in_loop([this]() {
                delete this;
            });
        } else {
            delete this;
        }
    }

    // 发送完数据后返�?
    void UdpConnection::close()
    {
        if (is_closing_ || closed_) {
            return; // 防止重复调用
        }
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        closed_ = true;
        auto *front = output_buffer_.front();
        if (lastState == ConnectionState::connecting || (front && front->readable_bytes() > 0)) {
            flush();
            return;
        }
        is_closing_ = true;
        // 使用延迟删除，避免在事件回调中直接删�?
        if (eventHandler_) {
            eventHandler_->queue_in_loop([this]() {
                delete this;
            });
        } else {
            delete this;
        }
    }

    void UdpConnection::set_connection_handler(ConnectionHandler * handler)
    {
        this->connectionHandler_ = handler;
    }

    void UdpConnection::on_read_event()
    {
        assert(state_ == ConnectionState::connected && connectionHandler_);
        replace_input_buffer(instance_->take_input_packet());

        bool ok = true;
        if (adapter_) {
            auto decoded = get_input_byte_buffer();
            ok = adapter_->on_recv(decoded) > 0;
            if (ok) {
                replace_input_buffer(std::move(decoded));
            }
        }
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
        auto *front = output_buffer_.front();
        if (front && !front->empty()) {
            flush();
        }
    }

    void UdpConnection::set_event_handler(EventHandler * eventHandler)
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

    ConnectionHandler *UdpConnection::get_connection_handler() const
    {
        return connectionHandler_;
    }

    void UdpConnection::attach_datagram_instance(UdpInstance * instance)
    {
        instance_ = instance;
    }

    void UdpConnection::set_datagram_state(ConnectionState state)
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

    void UdpConnection::on_timer(timer::Timer * timer)
    {
        if (is_closing_) {
            return; // 已在销毁过程中，忽�?timer 回调
        }

        if (!active_) {
            abort();
            return;
        }

        auto *front = output_buffer_.front();
        if (!front || front->empty()) {
            ++idle_cnt_;
        }

        // 第三次释�?
        if (idle_cnt_ >= 2) {
            active_ = false;
        }
    }

    void UdpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        assert(false);
    }

    bool UdpConnection::proc_one_buffer(const ::yuan::buffer::ByteBuffer & buffer)
    {
        auto packet = buffer.copy_readable();
        int ret = adapter_->on_write(packet);
        if (ret < 0) {
            return false;
        }

        if (ret < static_cast<int>(buffer.readable_bytes())) {
            auto remaining = buffer.copy_readable();
            remaining.consume(static_cast<std::size_t>(ret));
            pending_output_buffer_.push_back(std::make_unique< ::yuan::buffer::ByteBuffer>(std::move(remaining)));
        }
        return true;
    }

    void UdpConnection::process_pending_output_buffer()
    {
        auto *front = pending_output_buffer_.front();
        if (!front || front->empty()) {
            return;
        }

        std::size_t sz = pending_output_buffer_.size();
        for (int i = 0; i < sz; ++i) {
            front = pending_output_buffer_.front();
            if (!front || front->empty()) {
                return;
            }

            if (!proc_one_buffer(front->copy_readable())) {
                connectionHandler_->on_error(this);
                abort();
                return;
            }

            pending_output_buffer_.pop_front();
        }
    }
}
