#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__
#include "buffer/buffer_chain.h"
#include "connection.h"
#include "datagram_transport.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include <memory>

namespace yuan::net
{
    class UdpInstance;
    class UdpAdapter;

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

    private:
        bool active_;
        bool closed_;
        bool is_closing_;
        ConnectionState state_;
        int idle_cnt_;
        InetAddress address_;
        std::unique_ptr<UdpAdapter> adapter_;
        std::shared_ptr<ConnectionHandler> connectionHandlerOwner_;
        EventHandler *eventHandler_;
        UdpInstance *instance_;
        timer::Timer *alive_timer_;
        bool cleanup_done_ = false;
        bool close_notified_ = false;
        ::yuan::buffer::BufferChain pending_output_buffer_;
    };
}

#endif
