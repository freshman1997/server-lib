#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__
#include "buffer/linked_buffer.h"
#include "connection.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_task.h"

namespace yuan::net
{
    class UdpInstance;
    class UdpAdapter;

    class UdpConnection : public Connection, public timer::TimerTask
    {
    public:
        UdpConnection(const InetAddress &addr);

        UdpConnection(const InetAddress &addr, UdpAdapter *adapter);

        ~UdpConnection();

    public:
        virtual ConnectionState get_connection_state();

        virtual bool is_connected();

        virtual const InetAddress & get_remote_address();

        virtual buffer::Buffer * get_input_buff(bool take = false);

        virtual buffer::Buffer * get_output_buff(bool take = false);

        virtual void write(buffer::Buffer *buff);

        virtual void write_and_flush(buffer::Buffer *buff);

        virtual void flush();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        virtual ConnectionType get_conn_type();

        virtual Channel * get_channel();
        
        virtual void set_connection_handler(ConnectionHandler *handler);

        virtual ConnectionHandler * get_connection_handler();

        virtual void process_input_data(std::function<bool (buffer::Buffer *buff)> func, bool clear = true);

        virtual buffer::LinkedBuffer * get_input_linked_buffer();

        virtual buffer::LinkedBuffer * get_output_linked_buffer();
        
        virtual void forward(Connection *conn);

        virtual void set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler);

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    public:
        void set_instance_handler(UdpInstance *instance);

        void set_connection_state(ConnectionState state);

    public:
        virtual void on_timer(timer::Timer *timer);

    private:
        void do_close();

    private:
        bool active_;
        bool closed_;
        ConnectionState state_;
        int idle_cnt_;
        InetAddress address_;
        UdpAdapter *adapter_;
        ConnectionHandler *connectionHandler_;
        EventHandler *eventHandler_;
        UdpInstance *instance_;
        timer::Timer *alive_timer_;
        buffer::LinkedBuffer input_buffer_;
        buffer::LinkedBuffer output_buffer_;
    };
}

#endif