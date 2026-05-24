#include "net/iocp/iocp_tcp_engine.h"

#include "net/iocp/iocp_tcp_io.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket_ops.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace yuan::net
{
    namespace
    {
        constexpr int kInvalidSocket = -1;

        void close_engine_socket(int fd) noexcept
        {
            if (fd >= 0) {
                socket::close_fd(fd);
            }
        }

        InetAddress make_listen_address(const std::string &host, uint16_t port)
        {
            return InetAddress(host.empty() ? "0.0.0.0" : host, port);
        }

        int create_overlapped_tcp_socket(AddressFamily family)
        {
            return family == AddressFamily::ipv6
                ? socket::create_ipv6_overlapped_tcp_socket(false)
                : socket::create_ipv4_overlapped_tcp_socket(false);
        }
    }

    struct IocpTcpEngine::Operation
    {
        IocpOperation io{};
        std::weak_ptr<IocpTcpConnection> connection;
        int accepted_fd = kInvalidSocket;
        std::array<char, kIocpAcceptBufferBytes> accept_buffer{};
        std::vector<char> buffer;
        std::shared_ptr<const std::string> shared_buffer;
        bool drains_output = false;
        bool direct_output = false;
    };

    IocpTcpConnection::IocpTcpConnection(IocpTcpEngine &engine,
                                         int fd,
                                         InetAddress local_address,
                                         InetAddress remote_address)
        : engine_(&engine),
          fd_(fd),
          local_address_(std::move(local_address)),
          remote_address_(std::move(remote_address))
    {
    }

    IocpTcpConnection::~IocpTcpConnection()
    {
        close_now();
    }

    int IocpTcpConnection::fd() const noexcept
    {
        return fd_.load(std::memory_order_acquire);
    }

    bool IocpTcpConnection::closing() const noexcept
    {
        return context_.closing();
    }

    bool IocpTcpConnection::attach(IocpCompletionPort &port, uintptr_t key)
    {
        return context_.attach(fd_.load(std::memory_order_acquire), port, key);
    }

    bool IocpTcpConnection::complete(IocpOperation &operation) noexcept
    {
        return context_.complete_operation(operation);
    }

    bool IocpTcpConnection::post_recv(std::size_t buffer_bytes)
    {
        if (buffer_bytes == 0 || closing()) {
            return false;
        }

        auto *operation = new IocpTcpEngine::Operation{};
        operation->connection = self();
        operation->buffer.resize(buffer_bytes);
        if (!context_.begin_operation(operation->io, IocpOperationKind::recv, operation)) {
            delete operation;
            return false;
        }

        if (!IocpTcpIo::post_recv(context_.fd(),
                                  operation->buffer.data(),
                                  static_cast<uint32_t>(operation->buffer.size()),
                                  operation->io.native_overlapped())) {
            context_.complete_operation(operation->io);
            delete operation;
            return false;
        }
        return true;
    }

    bool IocpTcpConnection::send(const void *data, std::size_t size)
    {
        if (!data || size == 0 || closing()) {
            return false;
        }

        auto *operation = new IocpTcpEngine::Operation{};
        operation->connection = self();
        const auto *bytes = static_cast<const char *>(data);
        operation->buffer.assign(bytes, bytes + size);
        if (!context_.begin_operation(operation->io, IocpOperationKind::send, operation)) {
            delete operation;
            return false;
        }

        if (!IocpTcpIo::post_send(context_.fd(),
                                  operation->buffer.data(),
                                  static_cast<uint32_t>(operation->buffer.size()),
                                  operation->io.native_overlapped())) {
            context_.complete_operation(operation->io);
            delete operation;
            return false;
        }
        return true;
    }

    bool IocpTcpConnection::send(std::string data)
    {
        return send(data.data(), data.size());
    }

    bool IocpTcpConnection::send_shared(std::shared_ptr<const std::string> data)
    {
        if (!data || data->empty() || closing()) {
            return false;
        }

        auto *operation = new IocpTcpEngine::Operation{};
        operation->connection = self();
        operation->shared_buffer = std::move(data);
        if (!context_.begin_operation(operation->io, IocpOperationKind::send, operation)) {
            delete operation;
            return false;
        }

        if (!IocpTcpIo::post_send(context_.fd(),
                                  operation->shared_buffer->data(),
                                  static_cast<uint32_t>(operation->shared_buffer->size()),
                                  operation->io.native_overlapped())) {
            context_.complete_operation(operation->io);
            delete operation;
            return false;
        }
        return true;
    }

    ConnectionState IocpTcpConnection::get_connection_state() const
    {
        return state_.load(std::memory_order_acquire);
    }

    bool IocpTcpConnection::is_connected() const
    {
        return state_.load(std::memory_order_acquire) == ConnectionState::connected;
    }

    const InetAddress &IocpTcpConnection::get_remote_address() const
    {
        return remote_address_;
    }

    const InetAddress &IocpTcpConnection::get_local_address() const
    {
        return local_address_;
    }

    void IocpTcpConnection::write(const ::yuan::buffer::ByteBuffer &buffer)
    {
        const auto span = buffer.readable_span();
        if (!span.empty()) {
            append_output(span.data(), span.size());
        }
    }

    void IocpTcpConnection::write_owned(::yuan::buffer::ByteBuffer buffer)
    {
        if (!buffer.empty()) {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            output_buffer_.push_back(std::make_unique<::yuan::buffer::ByteBuffer>(std::move(buffer)));
        }
    }

    void IocpTcpConnection::write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
    {
        write(buffer);
        flush();
    }

    void IocpTcpConnection::write_owned_and_flush(::yuan::buffer::ByteBuffer buffer)
    {
        write_owned(std::move(buffer));
        flush();
    }

    void IocpTcpConnection::write_raw_and_flush(std::string_view data)
    {
        if (data.empty()) {
            return;
        }
        if (state_.load(std::memory_order_acquire) == ConnectionState::closed || closing() || get_ssl_handler()) {
            append_output(data);
            flush();
            return;
        }

        auto *operation = new IocpTcpEngine::Operation{};
        bool should_flush = false;
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            if (output_flush_pending_ || output_buffer_.readable_bytes() != 0) {
                ensure_output_chunk(data.size())->append(data);
                should_flush = !output_flush_pending_;
                delete operation;
                operation = nullptr;
            } else {
                operation->buffer.assign(data.data(), data.data() + data.size());
                operation->drains_output = true;
                operation->direct_output = true;
                output_flush_pending_ = true;
            }
        }

        if (!operation) {
            if (should_flush) {
                flush();
            }
            return;
        }

        operation->connection = self();
        if (!context_.begin_operation(operation->io, IocpOperationKind::send, operation)) {
            fail_output_send();
            delete operation;
            return;
        }

        if (!IocpTcpIo::post_send(context_.fd(),
                                  operation->buffer.data(),
                                  static_cast<uint32_t>(operation->buffer.size()),
                                  operation->io.native_overlapped())) {
            context_.complete_operation(operation->io);
            fail_output_send();
            delete operation;
        }
    }

    void IocpTcpConnection::flush()
    {
        if (state_.load(std::memory_order_acquire) == ConnectionState::closed || closing()) {
            return;
        }

        auto *operation = new IocpTcpEngine::Operation{};
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            if (output_flush_pending_) {
                delete operation;
                return;
            }

            for (;;) {
                auto *front = output_buffer_.front();
                if (!front) {
                    delete operation;
                    return;
                }
                if (!front->empty()) {
                    operation->buffer.assign(front->read_ptr(), front->read_ptr() + front->readable_bytes());
                    operation->drains_output = true;
                    output_flush_pending_ = true;
                    break;
                }
                output_buffer_.pop_front();
            }
        }

        operation->connection = self();
        if (!context_.begin_operation(operation->io, IocpOperationKind::send, operation)) {
            fail_output_send();
            delete operation;
            return;
        }

        if (!IocpTcpIo::post_send(context_.fd(),
                                  operation->buffer.data(),
                                  static_cast<uint32_t>(operation->buffer.size()),
                                  operation->io.native_overlapped())) {
            context_.complete_operation(operation->io);
            fail_output_send();
            delete operation;
        }
    }

    void IocpTcpConnection::abort()
    {
        close_now();
    }

    void IocpTcpConnection::close()
    {
        if (fd_.load(std::memory_order_acquire) < 0) {
            return;
        }

        bool should_flush = false;
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            if (state_.load(std::memory_order_acquire) == ConnectionState::closed) {
                return;
            }
            if (output_flush_pending_ || output_buffer_.readable_bytes() > 0) {
                state_.store(ConnectionState::closing, std::memory_order_release);
                close_after_output_ = true;
                should_flush = !output_flush_pending_;
            }
        }

        if (should_flush) {
            flush();
            return;
        }

        if (state_.load(std::memory_order_acquire) == ConnectionState::closing) {
            return;
        }

        if (engine_) {
            engine_->close_connection(self(), true, true);
        } else {
            close_now();
        }
    }

    void IocpTcpConnection::close_now(bool graceful_shutdown)
    {
        const int fd = fd_.exchange(-1, std::memory_order_acq_rel);
        if (fd < 0) {
            return;
        }

        if (graceful_shutdown) {
            (void)socket::shutdown_write(fd);
        }
        state_.store(ConnectionState::closed, std::memory_order_release);
        input_shutdown_.store(true, std::memory_order_release);
        output_shutdown_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            output_flush_pending_ = false;
            close_after_output_ = false;
            output_buffer_.clear();
        }
        context_.request_close(true);
        close_engine_socket(fd);
        if (engine_) {
            engine_->remove_connection(fd);
        }
    }

    bool IocpTcpConnection::shutdown_write()
    {
        const int fd = fd_.load(std::memory_order_acquire);
        if (fd < 0 || output_shutdown_.load(std::memory_order_acquire)) {
            return false;
        }
        const bool shutdown = socket::shutdown_write(fd);
        if (shutdown) {
            output_shutdown_.store(true, std::memory_order_release);
        }
        return shutdown;
    }

    bool IocpTcpConnection::input_shutdown() const
    {
        return input_shutdown_.load(std::memory_order_acquire);
    }

    Channel *IocpTcpConnection::stream_channel()
    {
        return nullptr;
    }

    const Channel *IocpTcpConnection::stream_channel() const
    {
        return nullptr;
    }

    void IocpTcpConnection::set_connection_handler(std::shared_ptr<ConnectionHandler> handler)
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        connection_handler_ = std::move(handler);
        has_connection_handler_.store(static_cast<bool>(connection_handler_), std::memory_order_release);
    }

    ConnectionHandler *IocpTcpConnection::get_connection_handler() const
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        return connection_handler_ ? &*connection_handler_ : nullptr;
    }

    std::shared_ptr<ConnectionHandler> IocpTcpConnection::get_connection_handler_owner() const
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        return connection_handler_;
    }

    void IocpTcpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        ssl_handler_ = std::move(sslHandler);
    }

    std::shared_ptr<SSLHandler> IocpTcpConnection::get_ssl_handler() const
    {
        return ssl_handler_;
    }

    void IocpTcpConnection::on_read_event()
    {
    }

    void IocpTcpConnection::on_write_event()
    {
    }

    void IocpTcpConnection::set_event_handler(EventHandler *eventHandler)
    {
        (void)eventHandler;
    }

    void IocpTcpConnection::set_user_data(UserData data)
    {
        std::lock_guard<std::mutex> lock(user_data_mutex_);
        user_data_ = std::move(data);
    }

    IocpTcpConnection::UserData IocpTcpConnection::user_data() const
    {
        std::lock_guard<std::mutex> lock(user_data_mutex_);
        return user_data_;
    }

    bool IocpTcpConnection::try_mark_read_dispatch_pending() noexcept
    {
        bool expected = false;
        return read_dispatch_pending_.compare_exchange_strong(expected,
                                                              true,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire);
    }

    void IocpTcpConnection::clear_read_dispatch_pending() noexcept
    {
        read_dispatch_pending_.store(false, std::memory_order_release);
    }

    void IocpTcpConnection::mark_defer_close_on_unconsumed_input() noexcept
    {
        defer_close_on_unconsumed_input_.store(true, std::memory_order_release);
    }

    std::shared_ptr<IocpTcpConnection> IocpTcpConnection::self()
    {
        return std::static_pointer_cast<IocpTcpConnection>(shared_from_this());
    }

    bool IocpTcpConnection::complete_output_send(std::size_t bytes, bool &close_after_output)
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex_);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            auto *front = output_buffer_.front();
            if (!front) {
                break;
            }
            const auto readable = front->readable_bytes();
            if (readable <= remaining) {
                remaining -= readable;
                output_buffer_.pop_front();
            } else {
                front->consume(remaining);
                remaining = 0;
            }
        }
        output_flush_pending_ = false;
        close_after_output = close_after_output_ && output_buffer_.readable_bytes() == 0;
        return output_buffer_.readable_bytes() > 0;
    }

    bool IocpTcpConnection::complete_direct_output_send(const char *data,
                                                        std::size_t size,
                                                        std::size_t bytes,
                                                        bool &close_after_output)
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex_);
        output_flush_pending_ = false;
        if (data && bytes < size) {
            auto remaining = std::make_unique<::yuan::buffer::ByteBuffer>(size - bytes);
            remaining->append(data + bytes, size - bytes);
            output_buffer_.push_front(std::move(remaining));
        }
        close_after_output = close_after_output_ && output_buffer_.readable_bytes() == 0;
        return output_buffer_.readable_bytes() > 0;
    }

    void IocpTcpConnection::fail_output_send()
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex_);
        output_flush_pending_ = false;
    }

    bool IocpTcpConnection::has_pending_output() const
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex_);
        return output_flush_pending_ || output_buffer_.readable_bytes() > 0;
    }

    bool IocpTcpConnection::mark_close_after_pending_output()
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex_);
        if (!output_flush_pending_ && output_buffer_.readable_bytes() == 0) {
            return false;
        }
        state_.store(ConnectionState::closing, std::memory_order_release);
        close_after_output_ = true;
        return !output_flush_pending_;
    }

    void IocpTcpConnection::notify_connected()
    {
        auto handler = get_connection_handler_owner();
        if (!handler && !has_event_waiter(ConnectionEvent::connected)) {
            return;
        }
        notify_event_waiters(ConnectionEvent::connected);
        if (handler) {
            handler->on_connected(self());
        }
    }

    void IocpTcpConnection::notify_read(const char *data, std::size_t size)
    {
        if (data && size > 0) {
            std::lock_guard<std::mutex> lock(input_buffer_mutex_);
            input_buffer_.append(data, size);
        }

        notify_event_waiters(ConnectionEvent::readable);
        if (!has_connection_handler_.load(std::memory_order_acquire)) {
            return;
        }
        auto handler = get_connection_handler_owner();
        if (handler) {
            handler->on_read(self());
        }
    }

    void IocpTcpConnection::notify_write()
    {
        auto handler = get_connection_handler_owner();
        if (!handler && !has_event_waiter(ConnectionEvent::writable)) {
            return;
        }
        notify_event_waiters(ConnectionEvent::writable);
        if (handler) {
            handler->on_write(self());
        }
    }

    void IocpTcpConnection::notify_error()
    {
        auto handler = get_connection_handler_owner();
        if (!handler && !has_event_waiter(ConnectionEvent::error)) {
            return;
        }
        notify_event_waiters(ConnectionEvent::error);
        if (handler) {
            handler->on_error(self());
        }
    }

    void IocpTcpConnection::notify_closed()
    {
        if (close_notified_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        auto handler = get_connection_handler_owner();
        if (!handler && !has_event_waiter(ConnectionEvent::closed)) {
            clear_event_waiters();
            return;
        }
        notify_event_waiters(ConnectionEvent::closed);
        if (handler) {
            handler->on_close(self());
        }
        clear_event_waiters();
    }

    IocpTcpEngine::~IocpTcpEngine()
    {
        stop();
    }

    bool IocpTcpEngine::listen(const std::string &host,
                               uint16_t port,
                               std::size_t worker_count,
                               IocpTcpEngineCallbacks callbacks,
                               std::size_t accept_count,
                               int backlog)
    {
        stop();
#ifdef _WIN32
        callbacks_ = std::move(callbacks);
        if (!port_.init()) {
            return false;
        }

        const InetAddress listen_addr = make_listen_address(host, port);
        listen_family_ = listen_addr.family();
        listener_ = create_overlapped_tcp_socket(listen_family_);
        if (listener_ == kInvalidSocket) {
            stop();
            return false;
        }

        socket::set_reuse_addr(listener_, true);
        socket::set_no_delay(listener_, true);
        const int listen_backlog = backlog > 0 ? backlog : SOMAXCONN;
        if (socket::bind(listener_, listen_addr) != 0 || socket::listen(listener_, listen_backlog) != 0) {
            stop();
            return false;
        }
        local_port_ = static_cast<uint16_t>(socket::get_local_address(listener_).get_port());

        if (!port_.associate_socket(listener_, reinterpret_cast<uintptr_t>(this)) ||
            !accept_ex_.load(listener_)) {
            stop();
            return false;
        }

        const std::size_t actual_workers = (std::max<std::size_t>)(1, worker_count);
        if (!dispatcher_.start_operations(port_, actual_workers, [this](IocpOperation &operation,
                                                                        const IocpCompletion &completion) {
                handle_completion(operation, completion);
            })) {
            stop();
            return false;
        }

        accept_count_ = accept_count != 0 ? accept_count : (std::max<std::size_t>)(16, actual_workers * 8);
        running_.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < accept_count_; ++i) {
            post_accept();
        }
        return true;
#else
        (void)host;
        (void)port;
        (void)worker_count;
        (void)callbacks;
        (void)accept_count;
        (void)backlog;
        return false;
#endif
    }

    bool IocpTcpEngine::connect(const std::string &host,
                                uint16_t port,
                                std::size_t worker_count,
                                IocpTcpEngineCallbacks callbacks)
    {
        stop();
#ifdef _WIN32
        callbacks_ = std::move(callbacks);
        if (!port_.init()) {
            return false;
        }

        const InetAddress remote_address(host, port);
        const auto family = remote_address.family();
        const int fd = create_overlapped_tcp_socket(family);
        if (fd == kInvalidSocket) {
            stop();
            return false;
        }

        const InetAddress local_bind(family == AddressFamily::ipv6 ? "::" : "0.0.0.0", 0);
        if (socket::bind(fd, local_bind) != 0) {
            close_engine_socket(fd);
            stop();
            return false;
        }
        socket::set_no_delay(fd, true);
        socket::set_keep_alive(fd, true);

        auto connection = std::make_shared<IocpTcpConnection>(*this,
                                                              fd,
                                                              socket::get_local_address(fd),
                                                              remote_address);
        connection->state_.store(ConnectionState::connecting, std::memory_order_release);
        if (!connection->attach(port_, reinterpret_cast<uintptr_t>(connection.get())) ||
            !connect_ex_.load(fd)) {
            connection->close_now();
            stop();
            return false;
        }

        const std::size_t actual_workers = (std::max<std::size_t>)(1, worker_count);
        if (!dispatcher_.start_operations(port_, actual_workers, [this](IocpOperation &operation,
                                                                        const IocpCompletion &completion) {
                handle_completion(operation, completion);
            })) {
            close_engine_socket(fd);
            stop();
            return false;
        }

        auto *operation = new Operation{};
        operation->connection = connection;
        if (!connection->context_.begin_operation(operation->io, IocpOperationKind::connect, operation)) {
            delete operation;
            connection->close_now();
            stop();
            return false;
        }

        const auto remote = remote_address.to_sockaddr();
        const int remote_len = remote_address.is_ipv6()
            ? static_cast<int>(sizeof(sockaddr_in6))
            : static_cast<int>(sizeof(sockaddr_in));
        if (!connect_ex_.post(fd,
                              reinterpret_cast<const sockaddr *>(&remote),
                              remote_len,
                              operation->io.native_overlapped())) {
            connection->context_.complete_operation(operation->io);
            delete operation;
            connection->close_now();
            stop();
            return false;
        }

        running_.store(true, std::memory_order_release);
        add_connection(connection);
        return true;
#else
        (void)host;
        (void)port;
        (void)worker_count;
        (void)callbacks;
        return false;
#endif
    }

    void IocpTcpEngine::stop()
    {
        running_.store(false, std::memory_order_release);
        if (listener_ != kInvalidSocket) {
            close_engine_socket(listener_);
            listener_ = kInvalidSocket;
        }

        std::vector<std::shared_ptr<IocpTcpConnection>> connections;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto &entry : connections_) {
                if (entry.second) {
                    connections.push_back(entry.second);
                }
            }
            connections_.clear();
        }
        for (auto &connection : connections) {
            if (connection) {
                connection->close();
            }
        }

        for (int i = 0; pending_accepts_.load(std::memory_order_acquire) != 0 && i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        dispatcher_.stop();
        port_.close();
        callbacks_ = {};
        local_port_ = 0;
        pending_accepts_.store(0, std::memory_order_release);
    }

    bool IocpTcpEngine::running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    uint16_t IocpTcpEngine::local_port() const noexcept
    {
        return local_port_;
    }

    bool IocpTcpEngine::post_accept()
    {
        if (!running_.load(std::memory_order_acquire) || listener_ == kInvalidSocket) {
            return false;
        }

        auto *operation = new Operation{};
        operation->io.reset(IocpOperationKind::accept, this, operation);
        operation->accepted_fd = create_overlapped_tcp_socket(listen_family_);
        if (operation->accepted_fd == kInvalidSocket) {
            delete operation;
            return false;
        }

        pending_accepts_.fetch_add(1, std::memory_order_acq_rel);
        if (!accept_ex_.post(listener_,
                             operation->accepted_fd,
                             operation->accept_buffer.data(),
                             operation->accept_buffer.size(),
                             operation->io.native_overlapped())) {
            pending_accepts_.fetch_sub(1, std::memory_order_acq_rel);
            close_engine_socket(operation->accepted_fd);
            delete operation;
            return false;
        }
        return true;
    }

    void IocpTcpEngine::handle_completion(IocpOperation &io, const IocpCompletion &completion)
    {
        auto *operation = static_cast<Operation *>(io.user_data);
        if (!operation) {
            return;
        }

        switch (io.kind) {
        case IocpOperationKind::accept:
            handle_accept(*operation, completion);
            break;
        case IocpOperationKind::connect:
            handle_connect(*operation, completion);
            break;
        case IocpOperationKind::recv:
            handle_recv(*operation, completion);
            break;
        case IocpOperationKind::send:
            handle_send(*operation, completion);
            break;
        default:
            delete operation;
            break;
        }
    }

    void IocpTcpEngine::handle_accept(Operation &operation, const IocpCompletion &completion)
    {
        pending_accepts_.fetch_sub(1, std::memory_order_acq_rel);
        const int accepted_fd = operation.accepted_fd;
        if (!completion.ok || !running_.load(std::memory_order_acquire) ||
            accepted_fd == kInvalidSocket ||
            !accept_ex_.update_accept_context(accepted_fd, listener_)) {
            close_engine_socket(accepted_fd);
            delete &operation;
            if (running_.load(std::memory_order_acquire)) {
                post_accept();
            }
            return;
        }

        socket::set_no_delay(accepted_fd, true);
        socket::set_keep_alive(accepted_fd, true);
        IocpAcceptedAddresses addresses;
        InetAddress local_address = socket::get_local_address(accepted_fd);
        InetAddress remote_address;
        if (accept_ex_.parse_addresses(operation.accept_buffer.data(),
                                       operation.accept_buffer.size(),
                                       addresses)) {
            local_address = InetAddress(addresses.local);
            remote_address = InetAddress(addresses.remote);
        }

        auto connection = std::make_shared<IocpTcpConnection>(*this,
                                                              accepted_fd,
                                                              std::move(local_address),
                                                              std::move(remote_address));
        if (!connection->attach(port_, reinterpret_cast<uintptr_t>(connection.get()))) {
            close_engine_socket(accepted_fd);
            delete &operation;
            if (running_.load(std::memory_order_acquire)) {
                post_accept();
            }
            return;
        }

        add_connection(connection);
        if (callbacks_.on_accept) {
            callbacks_.on_accept(connection);
        }
        connection->notify_connected();
        if (!connection->post_recv()) {
            close_connection(connection, true);
        }

        operation.accepted_fd = kInvalidSocket;
        delete &operation;
        if (running_.load(std::memory_order_acquire)) {
            post_accept();
        }
    }

    void IocpTcpEngine::handle_connect(Operation &operation, const IocpCompletion &completion)
    {
        auto connection = operation.connection.lock();
        if (!connection) {
            delete &operation;
            return;
        }

        const bool owned = connection->complete(operation.io);
        if (!owned) {
            delete &operation;
            return;
        }

        if (!completion.ok || !connect_ex_.update_connect_context(connection->fd())) {
            const auto error = completion.error;
            delete &operation;
            connection->notify_error();
            if (callbacks_.on_error) {
                callbacks_.on_error(connection, error);
            }
            close_connection(connection, true);
            return;
        }

        connection->local_address_ = socket::get_local_address(connection->fd());
        connection->state_.store(ConnectionState::connected, std::memory_order_release);
        delete &operation;
        if (callbacks_.on_connect) {
            callbacks_.on_connect(connection);
        }
        connection->notify_connected();
        if (!connection->post_recv()) {
            close_connection(connection, true);
        }
    }

    void IocpTcpEngine::handle_recv(Operation &operation, const IocpCompletion &completion)
    {
        auto connection = operation.connection.lock();
        if (!connection) {
            delete &operation;
            return;
        }

        const bool owned = connection->complete(operation.io);
        if (!owned) {
            delete &operation;
            return;
        }

        if (!completion.ok) {
            const auto error = completion.error;
            delete &operation;
            connection->notify_error();
            if (callbacks_.on_error) {
                callbacks_.on_error(connection, error);
            }
            close_connection(connection, true);
            return;
        }

        if (completion.bytes == 0) {
            delete &operation;
            connection->input_shutdown_.store(true, std::memory_order_release);
            connection->notify_event_waiters(ConnectionEvent::input_shutdown);
            auto handler = connection->get_connection_handler_owner();
            if (handler) {
                handler->on_input_shutdown(connection);
            }
            if (connection->read_dispatch_pending_.load(std::memory_order_acquire)) {
                return;
            }
            if (connection->mark_close_after_pending_output()) {
                connection->flush();
                return;
            }
            if (!connection->has_pending_output()) {
                if (connection->defer_close_on_unconsumed_input_.load(std::memory_order_acquire)) {
                    return;
                }
                close_connection(connection, true, true);
            }
            return;
        }

        if (connection->has_event_waiter(ConnectionEvent::readable)) {
            connection->mark_defer_close_on_unconsumed_input();
        }
        connection->notify_read(operation.buffer.data(), completion.bytes);
        if (callbacks_.on_read) {
            callbacks_.on_read(connection, operation.buffer.data(), completion.bytes);
        }
        delete &operation;
        if (!connection->closing() && running_.load(std::memory_order_acquire)) {
            if (!connection->post_recv()) {
                close_connection(connection, true, true);
            }
        }
    }

    void IocpTcpEngine::handle_send(Operation &operation, const IocpCompletion &completion)
    {
        auto connection = operation.connection.lock();
        if (!connection) {
            delete &operation;
            return;
        }

        const bool owned = connection->complete(operation.io);
        if (!owned) {
            delete &operation;
            return;
        }

        const uint32_t error = completion.error;
        const uint32_t bytes = completion.bytes;
        const bool drains_output = operation.drains_output;
        const bool direct_output = operation.direct_output;
        if (!completion.ok) {
            if (drains_output) {
                connection->fail_output_send();
            }
            delete &operation;
            connection->notify_error();
            if (callbacks_.on_error) {
                callbacks_.on_error(connection, error);
            }
            close_connection(connection, true);
            return;
        }

        if (drains_output) {
            bool close_after_output = false;
            const bool has_more_output = direct_output
                ? connection->complete_direct_output_send(operation.buffer.data(), operation.buffer.size(), bytes, close_after_output)
                : connection->complete_output_send(bytes, close_after_output);
            delete &operation;
            if (has_more_output) {
                connection->flush();
                return;
            }
            connection->notify_write();
            if (callbacks_.on_write) {
                callbacks_.on_write(connection, bytes);
            }
            if (connection->input_shutdown_.load(std::memory_order_acquire) &&
                connection->defer_close_on_unconsumed_input_.load(std::memory_order_acquire) &&
                !connection->has_pending_output()) {
                close_connection(connection, true, true);
                return;
            }
            if (close_after_output) {
                close_connection(connection, true, true);
            }
            return;
        }
        delete &operation;
        connection->notify_write();
        if (callbacks_.on_write) {
            callbacks_.on_write(connection, bytes);
        }
    }

    void IocpTcpEngine::add_connection(const std::shared_ptr<IocpTcpConnection> &connection)
    {
        if (!connection) {
            return;
        }
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[connection->fd()] = connection;
    }

    void IocpTcpEngine::remove_connection(int fd)
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(fd);
    }

    void IocpTcpEngine::close_connection(const std::shared_ptr<IocpTcpConnection> &connection,
                                         bool notify,
                                         bool graceful_shutdown)
    {
        if (!connection) {
            return;
        }
        connection->state_.store(ConnectionState::closed, std::memory_order_release);
        connection->input_shutdown_.store(true, std::memory_order_release);
        connection->output_shutdown_.store(true, std::memory_order_release);
        if (notify && callbacks_.on_close) {
            callbacks_.on_close(connection);
        }
        if (notify) {
            connection->notify_closed();
        }
        connection->close_now(graceful_shutdown);
    }
}
