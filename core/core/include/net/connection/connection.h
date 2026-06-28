#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include "buffer/byte_buffer.h"
#include "buffer/buffer_chain.h"
#include "net/handler/connection_handler.h"
#include "net/handler/event_handler.h"
#include "net/handler/select_handler.h"
#include "net/security/ssl_handler.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "base/spinlock.h"

namespace yuan::buffer
{
    class BufferChain;
}

namespace yuan::net
{
    namespace buffer
    {
        using ::yuan::buffer::BufferChain;
    }
}

namespace yuan::net
{
    class InetAddress;
    class Channel;
    class Socket;

    enum class ConnectionState {
        connecting,
        connected,
        closing,
        closed
    };

    enum class ConnectionEvent {
        connected,
        readable,
        writable,
        closed,
        error,
        input_shutdown,
    };

        static constexpr size_t DEFAULT_INPUT_BUFFER_SIZE = 16 * 1024;
        static constexpr size_t DEFAULT_MAX_PACKET_SIZE = 1024 * 1024 * 5;
        static constexpr size_t ET_DRAIN_MAX_BUFFER_SIZE = 4 * 1024 * 1024;

    using SslHandshakeCallback = std::function<void(bool success)>;

    class Connection : public SelectHandler, public std::enable_shared_from_this<Connection>
    {
    public:
        using EventWaiter = std::function<void(Connection &)>;
        using PendingReadResumer = std::function<void(std::coroutine_handle<>)>;
        using PendingReadEventResumer = std::function<void(ConnectionEvent)>;
        struct PendingRead
        {
            std::coroutine_handle<> handle{};
            PendingReadResumer resumer{};
            PendingReadEventResumer event_resumer{};
        };

        struct EventWaiterRegistration
        {
            ConnectionEvent event;
            EventWaiter waiter;
        };

        Connection()
            : max_packet_size_(DEFAULT_MAX_PACKET_SIZE),
              input_buffer_(DEFAULT_INPUT_BUFFER_SIZE)
        {
        }

        virtual ~Connection() = default;

        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;
        Connection(Connection &&) = delete;
        Connection &operator=(Connection &&) = delete;

        virtual ConnectionState get_connection_state() const = 0;
        virtual bool is_connected() const = 0;
        virtual const InetAddress &get_remote_address() const = 0;
        virtual const InetAddress &get_local_address() const = 0;

        virtual void write(const ::yuan::buffer::ByteBuffer &buffer) = 0;

        virtual void write_owned(::yuan::buffer::ByteBuffer buffer)
        {
            write(buffer);
        }

        virtual void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) = 0;

        virtual void write_owned_and_flush(::yuan::buffer::ByteBuffer buffer)
        {
            write_owned(std::move(buffer));
            flush();
        }

        virtual void write_raw_and_flush(std::string_view data)
        {
            append_output(data);
            flush();
        }

        virtual void flush() = 0;
        virtual void abort() = 0;
        virtual void close() = 0;
        virtual bool shutdown_write()
        {
            return false;
        }
        virtual bool input_shutdown() const
        {
            return false;
        }

        virtual void set_connection_handler(std::shared_ptr<ConnectionHandler> handler) = 0;
        virtual ConnectionHandler *get_connection_handler() const = 0;
        virtual std::shared_ptr<ConnectionHandler> get_connection_handler_owner() const
        {
            return nullptr;
        }

        virtual bool has_connection_handler() const
        {
            return static_cast<bool>(get_connection_handler_owner());
        }
        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) = 0;

        virtual std::shared_ptr<SSLHandler> get_ssl_handler() const
        {
            return nullptr;
        }

        virtual bool is_ssl_handshaking() const
        {
            return false;
        }

        virtual void set_ssl_handshaking(bool handshaking)
        {
            (void)handshaking;
        }

        virtual void set_ssl_handshake_callback(SslHandshakeCallback callback)
        {
            (void)callback;
        }

        uint64_t add_event_waiter(ConnectionEvent event, EventWaiter waiter)
        {
            if (!waiter) {
                return 0;
            }

            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            const auto id = next_waiter_id_++;
            waiter_count_.fetch_add(1, std::memory_order_release);
            waiters_.push_back(WaiterEntry{ id, event, std::move(waiter) });
            return id;
        }

        bool has_event_waiter(ConnectionEvent event) const
        {
            if (waiter_count_.load(std::memory_order_acquire) == 0) {
                return false;
            }
            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            return std::any_of(waiters_.begin(), waiters_.end(), [event](const auto &entry) {
                return entry.event == event;
            });
        }

        bool has_event_observer(ConnectionEvent event, bool external_callback = false) const
        {
            return external_callback || has_connection_handler() || has_event_waiter(event);
        }

        EventHandler *owner_event_handler() const noexcept
        {
            return owner_event_handler_.load(std::memory_order_acquire);
        }

        void detach_owner_event_handler() noexcept
        {
            set_owner_event_handler(nullptr);
        }

        bool is_in_owner_loop() const noexcept
        {
            auto *handler = owner_event_handler();
            return !handler || handler->is_in_loop_thread();
        }

        void dispatch_in_owner_loop(std::function<void()> fn)
        {
            if (!fn) {
                return;
            }

            auto *handler = owner_event_handler();
            if (handler && !handler->is_in_loop_thread()) {
                handler->queue_in_loop(std::move(fn));
                return;
            }
            fn();
        }

        void remove_event_waiter(uint64_t id)
        {
            if (id == 0) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            const auto old_size = waiters_.size();
            waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(), [id](const auto &entry) {
                return entry.id == id;
            }), waiters_.end());
            if (waiters_.size() != old_size) {
                waiter_count_.fetch_sub(1, std::memory_order_release);
            }
        }

        void add_event_waiters(EventWaiterRegistration *registrations,
                               std::size_t count,
                               std::vector<uint64_t> &ids)
        {
            if (!registrations || count == 0) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            ids.reserve(ids.size() + count);
            std::size_t added = 0;
            for (std::size_t i = 0; i < count; ++i) {
                auto &registration = registrations[i];
                if (!registration.waiter) {
                    ids.push_back(0);
                    continue;
                }
                const auto id = next_waiter_id_++;
                waiters_.push_back(WaiterEntry{ id, registration.event, std::move(registration.waiter) });
                ids.push_back(id);
                ++added;
            }
            if (added != 0) {
                waiter_count_.fetch_add(static_cast<uint32_t>(added), std::memory_order_release);
            }
        }

        std::size_t add_event_waiters(EventWaiterRegistration *registrations,
                                      std::size_t count,
                                      uint64_t *ids,
                                      std::size_t ids_capacity)
        {
            if (!registrations || count == 0 || !ids || ids_capacity == 0) {
                return 0;
            }

            const std::size_t limit = std::min(count, ids_capacity);
            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            std::size_t added = 0;
            for (std::size_t i = 0; i < limit; ++i) {
                auto &registration = registrations[i];
                if (!registration.waiter) {
                    ids[i] = 0;
                    continue;
                }
                const auto id = next_waiter_id_++;
                waiters_.push_back(WaiterEntry{ id, registration.event, std::move(registration.waiter) });
                ids[i] = id;
                ++added;
            }
            if (added != 0) {
                waiter_count_.fetch_add(static_cast<uint32_t>(added), std::memory_order_release);
            }
            return limit;
        }

        void remove_event_waiters(const std::vector<uint64_t> &ids)
        {
            if (ids.empty()) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            const auto old_size = waiters_.size();
            waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(), [&ids](const auto &entry) {
                return std::find(ids.begin(), ids.end(), entry.id) != ids.end();
            }), waiters_.end());
            const auto removed = old_size - waiters_.size();
            if (removed != 0) {
                waiter_count_.fetch_sub(static_cast<uint32_t>(removed), std::memory_order_release);
            }
        }

        void remove_event_waiters(const uint64_t *ids, std::size_t count)
        {
            if (!ids || count == 0) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            const auto old_size = waiters_.size();
            waiters_.erase(std::remove_if(waiters_.begin(), waiters_.end(), [ids, count](const auto &entry) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (ids[i] != 0 && ids[i] == entry.id) {
                        return true;
                    }
                }
                return false;
            }), waiters_.end());
            const auto removed = old_size - waiters_.size();
            if (removed != 0) {
                waiter_count_.fetch_sub(static_cast<uint32_t>(removed), std::memory_order_release);
            }
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            return input_buffer_.copy_readable();
        }

        size_t input_readable_bytes() const noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            return input_buffer_.readable_bytes();
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            auto byte_buffer = input_buffer_.copy_readable();
            input_buffer_.clear();
            shrink_input_buffer_if_idle();
            return byte_buffer;
        }

        ::yuan::buffer::ByteBuffer take_and_clear_input_byte_buffer()
        {
            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            auto byte_buffer = std::move(input_buffer_);
            input_buffer_.clear();
            shrink_input_buffer_if_idle();
            return byte_buffer;
        }

        void clear_input_buffer()
        {
            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            input_buffer_.clear();
            shrink_input_buffer_if_idle();
        }

        std::size_t output_readable_bytes() const noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(output_buffer_mutex_);
            return output_buffer_.readable_bytes();
        }

        void set_max_output_buffer_size(std::size_t size) noexcept
        {
            max_output_buffer_size_.store(size, std::memory_order_release);
        }

        std::size_t max_output_buffer_size() const noexcept
        {
            return max_output_buffer_size_.load(std::memory_order_acquire);
        }

        bool output_limit_exceeded() const noexcept
        {
            return output_limit_exceeded_.load(std::memory_order_acquire);
        }

        void clear_output_limit_exceeded() noexcept
        {
            output_limit_exceeded_.store(false, std::memory_order_release);
        }

        void append_output(std::string_view text)
        {
            if (!text.empty()) {
                std::lock_guard<yuan::base::Spinlock> lock(output_buffer_mutex_);
                if (!can_append_output_locked(text.size())) {
                    return;
                }
                ensure_output_chunk(text.size())->append(text);
                output_buffer_.account_append(text.size());
            }
        }

        void append_output(const char *data, std::size_t size)
        {
            if (data && size > 0) {
                std::lock_guard<yuan::base::Spinlock> lock(output_buffer_mutex_);
                if (!can_append_output_locked(size)) {
                    return;
                }
                ensure_output_chunk(size)->append(data, size);
                output_buffer_.account_append(size);
            }
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            const auto span = buffer.readable_span();
            if (!span.empty()) {
                std::lock_guard<yuan::base::Spinlock> lock(output_buffer_mutex_);
                if (!can_append_output_locked(span.size())) {
                    return;
                }
                ensure_output_chunk(span.size())->append(span);
                output_buffer_.account_append(span.size());
            }
        }

        void set_max_packet_size(size_t size)
        {
            assert(size > 0 && size <= 1024 * 1024 * 100);
            if (size == max_packet_size_) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> lock(input_buffer_mutex_);
            max_packet_size_ = size;
            if (input_buffer_.readable_bytes() == 0) {
                shrink_input_buffer_if_idle();
            }
        }

    protected:
        void set_owner_event_handler(EventHandler *handler) noexcept
        {
            owner_event_handler_.store(handler, std::memory_order_release);
        }

        void notify_connected_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            if (!handler && !has_event_waiter(ConnectionEvent::connected)) {
                return;
            }
            notify_event_waiters(ConnectionEvent::connected);
            if (handler) {
                handler->on_connected(*this);
            }
        }

        void notify_readable_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            auto pending = take_pending_read();
            if (pending.handle) {
                if (pending.resumer) {
                    pending.resumer(pending.handle);
                } else if (auto *event_handler = owner_event_handler()) {
                    event_handler->post_coroutine(pending.handle);
                }
            } else {
                notify_event_waiters(ConnectionEvent::readable);
            }
            if (handler) {
                handler->on_read(*this);
            }
        }

        void notify_writable_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            if (!handler && !has_event_waiter(ConnectionEvent::writable)) {
                return;
            }
            notify_event_waiters(ConnectionEvent::writable);
            if (handler) {
                handler->on_write(*this);
            }
        }

        void notify_error_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            complete_pending_read_event(ConnectionEvent::error);
            if (!handler && !has_event_waiter(ConnectionEvent::error)) {
                return;
            }
            notify_event_waiters(ConnectionEvent::error);
            if (handler) {
                handler->on_error(*this);
            }
        }

        void notify_input_shutdown_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            complete_pending_read_event(ConnectionEvent::input_shutdown);
            if (!handler && !has_event_waiter(ConnectionEvent::input_shutdown)) {
                return;
            }
            notify_event_waiters(ConnectionEvent::input_shutdown);
            if (handler) {
                handler->on_input_shutdown(*this);
            }
        }

        void notify_closed_event()
        {
            auto handler = has_connection_handler()
                ? get_connection_handler_owner()
                : std::shared_ptr<ConnectionHandler>{};
            complete_pending_read_event(ConnectionEvent::closed);
            if (!handler && !has_event_waiter(ConnectionEvent::closed)) {
                clear_event_waiters();
                return;
            }
            notify_event_waiters(ConnectionEvent::closed);
            if (handler) {
                handler->on_close(*this);
            }
            clear_event_waiters();
        }

        bool complete_pending_read_event(ConnectionEvent event)
        {
            auto pending = take_pending_read();
            if (!pending.handle) {
                return false;
            }
            if (pending.event_resumer) {
                pending.event_resumer(event);
            } else if (auto *event_handler = owner_event_handler()) {
                event_handler->post_coroutine(pending.handle);
            }
            return true;
        }

        size_t preferred_input_buffer_capacity() const noexcept
        {
            return std::min<size_t>(DEFAULT_INPUT_BUFFER_SIZE, max_packet_size_);
        }

        void shrink_input_buffer_if_idle()
        {
            const size_t preferred_capacity = preferred_input_buffer_capacity();
            if (input_buffer_.readable_bytes() == 0 && input_buffer_.capacity() > preferred_capacity) {
                input_buffer_ = ::yuan::buffer::ByteBuffer(preferred_capacity);
            }
        }

        bool grow_input_buffer()
        {
            if (input_buffer_.writable_bytes() > 0) {
                return true;
            }

            if (input_buffer_.read_offset() > 0) {
                input_buffer_.compact();
                if (input_buffer_.writable_bytes() > 0) {
                    return true;
                }
            }

            if (input_buffer_.capacity() >= max_packet_size_) {
                return false;
            }

            const size_t doubled = input_buffer_.capacity() * 2;
            const size_t next_capacity = std::min<size_t>(max_packet_size_, std::max<size_t>(doubled, preferred_input_buffer_capacity()));
            input_buffer_.reserve(next_capacity);
            return input_buffer_.writable_bytes() > 0;
        }

        bool drain_grow_input_buffer()
        {
            if (input_buffer_.writable_bytes() > 0) {
                return true;
            }

            if (input_buffer_.read_offset() > 0) {
                input_buffer_.compact();
                if (input_buffer_.writable_bytes() > 0) {
                    return true;
                }
            }

            if (input_buffer_.capacity() >= ET_DRAIN_MAX_BUFFER_SIZE) {
                return false;
            }
            const size_t doubled = input_buffer_.capacity() * 2;
            const size_t next_capacity = std::min<size_t>(ET_DRAIN_MAX_BUFFER_SIZE, doubled);
            input_buffer_.reserve(next_capacity);
            return input_buffer_.writable_bytes() > 0;
        }

        ::yuan::buffer::ByteBuffer *ensure_output_chunk(std::size_t capacity = ::yuan::buffer::ByteBuffer::kDefaultCapacity)
        {
            auto *chunk = output_buffer_.back();
            if (!chunk || chunk->writable_bytes() < capacity) {
                chunk = output_buffer_.emplace_back(std::max(capacity, ::yuan::buffer::ByteBuffer::kDefaultCapacity));
            }
            return chunk;
        }

        bool can_append_output_locked(std::size_t bytes)
        {
            const auto limit = max_output_buffer_size_.load(std::memory_order_acquire);
            if (limit == 0) {
                return true;
            }
            if (bytes > limit || output_buffer_.readable_bytes() > limit - bytes) {
                output_limit_exceeded_.store(true, std::memory_order_release);
                return false;
            }
            return true;
        }

        void replace_input_buffer(::yuan::buffer::ByteBuffer buffer)
        {
            input_buffer_ = std::move(buffer);
            max_packet_size_ = std::max<size_t>(max_packet_size_, input_buffer_.capacity());
        }

        void notify_event_waiters(ConnectionEvent event)
        {
            if (waiter_count_.load(std::memory_order_acquire) == 0) {
                return;
            }

            std::vector<EventWaiter> callbacks;
            {
                std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
                std::size_t removed = 0;
                for (auto it = waiters_.begin(); it != waiters_.end();) {
                    if (it->event == event) {
                        callbacks.push_back(std::move(it->callback));
                        it = waiters_.erase(it);
                        ++removed;
                    } else {
                        ++it;
                    }
                }
                if (removed != 0) {
                    waiter_count_.fetch_sub(static_cast<uint32_t>(removed), std::memory_order_release);
                }
            }

            if (callbacks.empty()) {
                return;
            }

            for (auto &callback : callbacks) {
                if (callback) {
                    callback(*this);
                }
            }
        }

        void clear_event_waiters()
        {
            std::lock_guard<yuan::base::Spinlock> lock(waiter_mutex_);
            waiters_.clear();
            waiter_count_.store(0, std::memory_order_release);
        }

    public:
        std::coroutine_handle<> pending_read_coroutine() const noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(pending_read_mutex_);
            return pending_read_coroutine_;
        }

        void set_pending_read_coroutine(std::coroutine_handle<> handle,
                                        PendingReadResumer resumer = {},
                                        PendingReadEventResumer event_resumer = {}) noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(pending_read_mutex_);
            pending_read_coroutine_ = handle;
            pending_read_resumer_ = std::move(resumer);
            pending_read_event_resumer_ = std::move(event_resumer);
        }

        void clear_pending_read_coroutine() noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(pending_read_mutex_);
            pending_read_coroutine_ = nullptr;
            pending_read_resumer_ = {};
            pending_read_event_resumer_ = {};
        }

        std::coroutine_handle<> take_pending_read_coroutine() noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(pending_read_mutex_);
            auto h = pending_read_coroutine_;
            pending_read_coroutine_ = nullptr;
            pending_read_resumer_ = {};
            pending_read_event_resumer_ = {};
            return h;
        }

        PendingRead take_pending_read() noexcept
        {
            std::lock_guard<yuan::base::Spinlock> lock(pending_read_mutex_);
            auto h = pending_read_coroutine_;
            auto resumer = std::move(pending_read_resumer_);
            auto event_resumer = std::move(pending_read_event_resumer_);
            pending_read_coroutine_ = nullptr;
            pending_read_resumer_ = {};
            pending_read_event_resumer_ = {};
            return PendingRead{ h, std::move(resumer), std::move(event_resumer) };
        }

    protected:

        size_t max_packet_size_;
        mutable yuan::base::Spinlock input_buffer_mutex_;
        ::yuan::buffer::ByteBuffer input_buffer_;
        mutable yuan::base::Spinlock output_buffer_mutex_;
        ::yuan::buffer::BufferChain output_buffer_;
        std::atomic_size_t max_output_buffer_size_{0};
        std::atomic_bool output_limit_exceeded_{false};

    private:
        struct WaiterEntry
        {
            uint64_t id;
            ConnectionEvent event;
            EventWaiter callback;
        };

        mutable yuan::base::Spinlock waiter_mutex_;
        std::atomic_uint32_t waiter_count_{0};
        uint64_t next_waiter_id_ = 1;
        std::vector<WaiterEntry> waiters_;
        mutable yuan::base::Spinlock pending_read_mutex_;
        std::coroutine_handle<> pending_read_coroutine_;
        PendingReadResumer pending_read_resumer_;
        PendingReadEventResumer pending_read_event_resumer_;
        std::atomic<EventHandler *> owner_event_handler_{nullptr};
    };

    using ConnectionPtr = std::shared_ptr<Connection>;
}

#endif
