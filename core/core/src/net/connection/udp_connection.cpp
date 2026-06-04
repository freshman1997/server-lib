#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/handler/event_handler.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/acceptor/udp/adapter.h"
#include "platform/native_platform.h"
#include "base/owner_ptr.h"
#include "logger.h"
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
        connectionHandlerOwner_.reset();
        eventHandler_ = nullptr;
        instance_ = nullptr;
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
        alive_timer_.cancel();
        alive_timer_.reset();

        if (adapter_) {
            adapter_->on_release();
            adapter_.reset();
        }

        // Guard against double cleanup when close/abort already removed the connection.
        if (instance_ && !cleanup_done_ && !instance_->is_closing()) {
            instance_->on_connection_close(this);
            instance_ = nullptr;
        }

        if (connectionHandlerOwner_ && !close_notified_) {
            LOG_WARN("udp connection destroyed without close notification, ip: {}, port: {}",
                     address_.get_ip(), address_.get_port());
        }
        connectionHandlerOwner_.reset();
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

    const InetAddress &UdpConnection::get_local_address() const
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
                if (connectionHandlerOwner_) {
                    notify_event_waiters(ConnectionEvent::error);
                    connectionHandlerOwner_->on_error(*this);
                }
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

    void UdpConnection::write_owned(::yuan::buffer::ByteBuffer buffer)
    {
        if (buffer.empty() || closed_) {
            return;
        }

        if (adapter_) {
            if (!proc_one_buffer(buffer)) {
                if (connectionHandlerOwner_) {
                    notify_event_waiters(ConnectionEvent::error);
                    connectionHandlerOwner_->on_error(*this);
                }
                abort();
                return;
            }
        } else {
            output_buffer_.push_back(std::make_unique<::yuan::buffer::ByteBuffer>(std::move(buffer)));
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

    void UdpConnection::write_owned_and_flush(::yuan::buffer::ByteBuffer buffer)
    {
        if (buffer.empty() || closed_) {
            return;
        }

        write_owned(std::move(buffer));
        flush();
    }
    void UdpConnection::flush()
    {
        assert(connectionHandlerOwner_);

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
                int sent = instance_->on_send(this, *front);
                if (sent > 0) {
                    front->consume(static_cast<std::size_t>(sent));
                    if (front->empty()) {
                        output_buffer_.pop_front();
                    } else {
                        instance_->request_write(this);
                        return;
                    }
                    ++i;
                } else if (sent < 0) {
#ifdef _WIN32
                    int err = platform::GetLastNativeError();
                    if (!platform::IsNativeRetryableError(err)) {
#else
                    int err = platform::GetLastNativeError();
                    if (!platform::IsNativeRetryableError(err)) {
#endif
                        abort();
                        return;
                    }
                } else {
                    instance_->request_write(this);
                    break;
                }
            }
        }

        front = output_buffer_.front();
        if ((!front || front->empty()) && closed_ && !is_closing_) {
            is_closing_ = true;
            // 使用延迟删除，避免在事件回调中直接删�?
            do_close();
        }
    }

    // 丢弃所有未发送的数据，立即标记销�?
    void UdpConnection::abort()
    {
        if (is_closing_ || state_ == ConnectionState::closed) {
            return;
        }
        is_closing_ = true;
        closed_ = true;
        state_ = ConnectionState::closed;
        output_buffer_.clear();
        pending_output_buffer_.clear();
        do_close();
    }

    // Graceful close drains queued datagrams before notifying close.
    void UdpConnection::close()
    {
        if (is_closing_ || state_ == ConnectionState::closed) {
            return;
        }
        state_ = ConnectionState::closing;
        closed_ = true;
        process_pending_output_buffer();
        auto *front = output_buffer_.front();
        if (front && front->readable_bytes() > 0) {
            flush();
            return;
        }
        is_closing_ = true;
        do_close();
    }

    void UdpConnection::set_connection_handler(std::shared_ptr<ConnectionHandler> handler)
    {
        connectionHandlerOwner_ = std::move(handler);
    }

    void UdpConnection::on_read_event()
    {
        [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
        auto *handler = yuan::base::owner_ptr(handler_owner);
        assert(state_ == ConnectionState::connected && handler);
        replace_input_buffer(instance_->take_input_packet());

        bool ok = true;
        if (adapter_) {
            auto decoded = take_and_clear_input_byte_buffer();
            ok = adapter_->on_recv(decoded) > 0;
            if (ok) {
                replace_input_buffer(std::move(decoded));
            }
        }
        if (ok) {
            active_ = true;
            notify_event_waiters(ConnectionEvent::readable);
            handler->on_read(*this);
        } else {
            notify_event_waiters(ConnectionEvent::error);
            abort();
        }
    }

    void UdpConnection::on_write_event()
    {
        [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
        auto *handler = yuan::base::owner_ptr(handler_owner);
        if (handler) {
            handler->on_write(*this);
        }
        process_pending_output_buffer();
        auto *front = output_buffer_.front();
        if (front && !front->empty()) {
            flush();
        }
        if (!front || front->empty()) {
            notify_event_waiters(ConnectionEvent::writable);
        }
    }

    void UdpConnection::set_event_handler(EventHandler * eventHandler)
    {
        if (eventHandler_ && eventHandler_ != eventHandler) {
            LOG_WARN("udp connection event handler switched, ip: {}, port: {}", address_.get_ip(), address_.get_port());
        }
        eventHandler_ = eventHandler;
    }

    void UdpConnection::do_close()
    {
        if (cleanup_done_) {
            return;
        }
        cleanup_done_ = true;
        is_closing_ = true;
        alive_timer_.cancel();
        alive_timer_.reset();
        auto self = std::static_pointer_cast<UdpConnection>(shared_from_this());
        [[maybe_unused]] auto handler_owner = std::move(connectionHandlerOwner_);
        auto *handler = yuan::base::owner_ptr(handler_owner);
        if (handler && !close_notified_) {
            close_notified_ = true;
            notify_event_waiters(ConnectionEvent::closed);
            handler->on_close(*this);
        }
        if (instance_ && !instance_->is_closing()) {
            instance_->on_connection_close(self);
        }
    }

    ConnectionHandler *UdpConnection::get_connection_handler() const
    {
        return yuan::base::owner_ptr(connectionHandlerOwner_);
    }

    void UdpConnection::attach_datagram_instance(UdpInstance * instance)
    {
        instance_ = instance;
    }

    void UdpConnection::set_datagram_state(ConnectionState state)
    {
        if (state_ == state) {
            return;
        }

        state_ = state;
        if (state_ == ConnectionState::connected) {
            [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
            auto *handler = yuan::base::owner_ptr(handler_owner);
            if (handler) {
                notify_event_waiters(ConnectionEvent::connected);
                handler->on_connected(*this);
            }
            if (instance_ && instance_->get_timer_manager()) {
                alive_timer_ = instance_->get_timer_manager()->every(0, 10 * 1000, this);
            }
        }
    }

    void UdpConnection::on_timer(timer::Timer * timer)
    {
        if (is_closing_) {
            return; // 已在销毁过程中，忽�?timer 回调
        }

        if (!active_) {
            close();
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
        int ret = adapter_->on_write(buffer);
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

            if (!proc_one_buffer(*front)) {
                [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
                auto *handler = yuan::base::owner_ptr(handler_owner);
                if (handler) {
                    notify_event_waiters(ConnectionEvent::error);
                    handler->on_error(*this);
                }
                abort();
                return;
            }

            pending_output_buffer_.pop_front();
        }
    }
}
