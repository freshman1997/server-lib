#ifndef __IOCP_TCP_ENGINE_H__
#define __IOCP_TCP_ENGINE_H__

#include "net/iocp/iocp_accept.h"
#include "net/iocp/iocp_connect.h"
#include "net/iocp/iocp_completion_port.h"
#include "net/iocp/iocp_dispatcher.h"
#include "net/iocp/iocp_operation.h"
#include "net/iocp/iocp_socket_context.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/socket/inet_address.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net
{
    class IocpTcpEngine;

    class IocpTcpConnection : public Connection, public StreamTransport
    {
    public:
        using UserData = std::shared_ptr<void>;

        IocpTcpConnection(IocpTcpEngine &engine,
                          int fd,
                          InetAddress local_address,
                          InetAddress remote_address);
        ~IocpTcpConnection();

        IocpTcpConnection(const IocpTcpConnection &) = delete;
        IocpTcpConnection &operator=(const IocpTcpConnection &) = delete;

        int fd() const noexcept;
        bool closing() const noexcept;
        bool post_recv(std::size_t buffer_bytes = 4096);
        bool send(const void *data, std::size_t size);
        bool send(std::string data);
        bool send_shared(std::shared_ptr<const std::string> data);

        ConnectionState get_connection_state() const override;
        bool is_connected() const override;
        const InetAddress &get_remote_address() const override;
        const InetAddress &get_local_address() const override;
        void write(const ::yuan::buffer::ByteBuffer &buffer) override;
        void write_owned(::yuan::buffer::ByteBuffer buffer) override;
        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer) override;
        void write_owned_and_flush(::yuan::buffer::ByteBuffer buffer) override;
        void write_raw_and_flush(std::string_view data) override;
        void flush() override;
        void abort() override;
        void close() override;
        bool shutdown_write() override;
        bool input_shutdown() const override;
        Channel *stream_channel() override;
        const Channel *stream_channel() const override;
        void set_connection_handler(std::shared_ptr<ConnectionHandler> handler) override;
        ConnectionHandler *get_connection_handler() const override;
        std::shared_ptr<ConnectionHandler> get_connection_handler_owner() const override;
        void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler) override;
        std::shared_ptr<SSLHandler> get_ssl_handler() const override;
        void on_read_event() override;
        void on_write_event() override;
        void set_event_handler(EventHandler *eventHandler) override;

        void set_user_data(UserData data);
        UserData user_data() const;
        bool try_mark_read_dispatch_pending() noexcept;
        void clear_read_dispatch_pending() noexcept;
        void mark_defer_close_on_unconsumed_input() noexcept;

    private:
        friend class IocpTcpEngine;

        bool attach(IocpCompletionPort &port, uintptr_t key);
        bool complete(IocpOperation &operation) noexcept;
        std::shared_ptr<IocpTcpConnection> self();
        bool complete_output_send(std::size_t bytes, bool &close_after_output);
        bool complete_direct_output_send(const char *data, std::size_t size, std::size_t bytes, bool &close_after_output);
        void fail_output_send();
        bool has_pending_output() const;
        bool mark_close_after_pending_output();
        void close_now(bool graceful_shutdown = false);
        void notify_connected();
        void notify_read(const char *data, std::size_t size);
        void notify_write();
        void notify_error();
        void notify_closed();

        IocpTcpEngine *engine_ = nullptr;
        std::atomic_int fd_{-1};
        std::atomic<ConnectionState> state_{ConnectionState::connected};
        InetAddress local_address_;
        InetAddress remote_address_;
        IocpSocketContext context_;
        mutable std::mutex user_data_mutex_;
        UserData user_data_;
        mutable std::mutex handler_mutex_;
        std::shared_ptr<ConnectionHandler> connection_handler_;
        std::atomic_bool has_connection_handler_{false};
        std::shared_ptr<SSLHandler> ssl_handler_;
        bool output_flush_pending_ = false;
        bool close_after_output_ = false;
        std::atomic_bool input_shutdown_{false};
        std::atomic_bool output_shutdown_{false};
        std::atomic_bool close_notified_{false};
        std::atomic_bool read_dispatch_pending_{false};
        std::atomic_bool defer_close_on_unconsumed_input_{false};
    };

    struct IocpTcpEngineCallbacks
    {
        std::function<void(const std::shared_ptr<IocpTcpConnection> &)> on_accept;
        std::function<void(const std::shared_ptr<IocpTcpConnection> &)> on_connect;
        std::function<void(const std::shared_ptr<IocpTcpConnection> &, const char *, std::size_t)> on_read;
        std::function<void(const std::shared_ptr<IocpTcpConnection> &, std::size_t)> on_write;
        std::function<void(const std::shared_ptr<IocpTcpConnection> &, uint32_t)> on_error;
        std::function<void(const std::shared_ptr<IocpTcpConnection> &)> on_close;
    };

    class IocpTcpEngine
    {
    public:
        IocpTcpEngine() = default;
        ~IocpTcpEngine();

        IocpTcpEngine(const IocpTcpEngine &) = delete;
        IocpTcpEngine &operator=(const IocpTcpEngine &) = delete;

        bool listen(const std::string &host,
                    uint16_t port,
                    std::size_t worker_count,
                    IocpTcpEngineCallbacks callbacks,
                    std::size_t accept_count = 0,
                    int backlog = 128,
                    std::size_t completion_batch_size = 1);
        bool connect(const std::string &host,
                     uint16_t port,
                     std::size_t worker_count,
                     IocpTcpEngineCallbacks callbacks);
        void stop();

        bool running() const noexcept;
        uint16_t local_port() const noexcept;

    private:
        friend class IocpTcpConnection;
        struct Operation;

        bool post_accept();
        void handle_completion(IocpOperation &operation, const IocpCompletion &completion);
        void handle_accept(Operation &operation, const IocpCompletion &completion);
        void handle_connect(Operation &operation, const IocpCompletion &completion);
        void handle_recv(Operation &operation, const IocpCompletion &completion);
        void handle_send(Operation &operation, const IocpCompletion &completion);
        void add_connection(const std::shared_ptr<IocpTcpConnection> &connection);
        void remove_connection(int fd);
        void close_connection(const std::shared_ptr<IocpTcpConnection> &connection, bool notify, bool graceful_shutdown = false);

        IocpCompletionPort port_;
        IocpDispatcher dispatcher_;
        IocpAcceptEx accept_ex_;
        IocpConnectEx connect_ex_;
        IocpTcpEngineCallbacks callbacks_;
        int listener_ = -1;
        uint16_t local_port_ = 0;
        AddressFamily listen_family_ = AddressFamily::ipv4;
        std::atomic_bool running_{false};
        std::atomic_uint32_t pending_accepts_{0};
        std::size_t accept_count_ = 0;
        std::mutex connections_mutex_;
        std::unordered_map<int, std::shared_ptr<IocpTcpConnection>> connections_;
    };
}

#endif
