#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__
#include "buffer/buffer_chain.h"
#include "connection.h"
#include "datagram_transport.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_handle.h"
#include "timer/timer_task.h"
#include <atomic>
#include <cstdint>
#include <memory>

namespace yuan::net
{
    class UdpInstance;
    class UdpAdapter;

    struct UdpConnectionOptions
    {
        std::size_t max_datagram_size = 1472;
        std::size_t max_pending_output_bytes = 256 * 1024;
        std::size_t max_pending_output_datagrams = 1024;
        std::uint32_t idle_check_interval_ms = 10000;
        std::uint32_t idle_timeout_checks = 2;
    };

    struct UdpConnectionMetrics
    {
        std::uint64_t datagrams_read = 0;
        std::uint64_t bytes_read = 0;
        std::uint64_t datagrams_written = 0;
        std::uint64_t bytes_written = 0;
        std::uint64_t datagrams_dropped = 0;
        std::uint64_t bytes_dropped = 0;
        std::uint64_t send_errors = 0;
        std::uint64_t receive_errors = 0;
        std::uint64_t active_connections = 0;
        std::uint64_t created_connections = 0;
        std::uint64_t closed_connections = 0;
    };

    class UdpConnection : public Connection, public DatagramTransport, public timer::TimerTask
    {
    public:
        UdpConnection(const InetAddress &addr);

        UdpConnection(const InetAddress &addr, UdpAdapter *adapter);

        ~UdpConnection();

    public:
        virtual ConnectionState get_connection_state() const override;

        virtual bool is_connected() const override;

        virtual const InetAddress &get_remote_address() const override;
        virtual const InetAddress &get_local_address() const override;

        const InetAddress &peer_address() const override;
        void attach_datagram_instance(UdpInstance *instance) override;
        void set_datagram_state(ConnectionState state) override;
        UdpInstance *datagram_instance() const
        {
            return instance_;
        }

        virtual void write(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void write_owned(::yuan::buffer::ByteBuffer buffer) override;

        virtual void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer);

        virtual void write_owned_and_flush(::yuan::buffer::ByteBuffer buffer) override;

        [[nodiscard]] std::size_t pending_output_bytes() const noexcept { return pending_output_bytes_; }
        [[nodiscard]] std::size_t pending_output_datagrams() const noexcept;
        [[nodiscard]] bool output_over_limit() const noexcept { return output_over_limit_; }

        virtual void flush();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();
        virtual bool shutdown_write() override { return false; }
        virtual bool input_shutdown() const override { return false; }

        virtual void set_connection_handler(std::shared_ptr<ConnectionHandler> handler) override;

        virtual ConnectionHandler *get_connection_handler() const override;
        virtual std::shared_ptr<ConnectionHandler> get_connection_handler_owner() const override
        {
            return connectionHandlerOwner_;
        }

        bool has_connection_handler() const override
        {
            return static_cast<bool>(connectionHandlerOwner_);
        }

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler);

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    public:
    public:
        virtual void on_timer(timer::Timer *timer);

    private:
        void do_close();

        void process_pending_output_buffer();

        bool proc_one_buffer(const ::yuan::buffer::ByteBuffer &buffer);
        bool enqueue_output(std::unique_ptr<::yuan::buffer::ByteBuffer> buffer);
        bool can_enqueue_output(std::size_t bytes) const;
        void account_drop(std::size_t bytes);
        void account_send(std::size_t bytes);
        void account_send_error();

    private:
        bool active_;
        bool closed_;
        bool is_closing_;
        ConnectionState state_;
        int idle_cnt_;
        InetAddress remote_address_;
        InetAddress local_address_;
        std::unique_ptr<UdpAdapter> adapter_;
        std::shared_ptr<ConnectionHandler> connectionHandlerOwner_;
        EventHandler *eventHandler_;
        UdpInstance *instance_;
        timer::TimerHandle alive_timer_;
        bool cleanup_done_ = false;
        bool close_notified_ = false;
        ::yuan::buffer::BufferChain pending_output_buffer_;
        std::size_t pending_output_bytes_ = 0;
        bool output_over_limit_ = false;
    };
}

#endif
