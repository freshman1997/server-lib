#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__
#include "../../../buffer/linked_buffer.h"
#include "../../base/connection/connection.h"
#include "../../base/socket/inet_address.h"
#include "../../../timer/timer.h"
#include "../../../timer/timer_task.h"

namespace net
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

        virtual Buffer * get_input_buff(bool take = false);

        virtual Buffer * get_output_buff(bool take = false);

        virtual void write(Buffer *buff);

        virtual void write_and_flush(Buffer *buff);

        virtual void send();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        virtual ConnectionType get_conn_type();

        virtual Channel * get_channel();
        
        virtual void set_connection_handler(ConnectionHandler *handler);

        virtual ConnectionHandler * get_connection_handler();

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
        LinkedBuffer input_buffer_;
        LinkedBuffer output_buffer_;
    };
}

#endif