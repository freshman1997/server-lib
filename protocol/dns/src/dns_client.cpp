#include "dns_client.h"
#include "event/event_loop.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/udp_connection.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/acceptor/udp_acceptor.h"
#include <iostream>

namespace yuan::net::dns 
{
    DnsClient::DnsClient()
    {
        retry_cnt_ = 0;
    }

    DnsClient::~DnsClient()
    {

    }

    void DnsClient::on_connected(Connection *conn)
    {
        conn->set_connection_handler(this);
    }

    void DnsClient::on_error(Connection *conn)
    {
        std::cout << "connection error!\n";
    }

    void DnsClient::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        std::string str(buff->peek(), buff->peek() + buff->readable_bytes());
        std::cout << "xxxxxxx: " << str << '\n';
        conn->close();
        ev_loop_->quit();
    }

    void DnsClient::on_write(Connection *conn)
    {
        if (retry_cnt_ > 0) {
            return;
        }

        retry_cnt_++;
        auto buf = conn->get_output_linked_buffer()->get_current_buffer();
        //std::string daoke = R"({"MsgID":943024804,"Cmd":24,"MsgBody":{"Uin":261281,"RoleID":"369691744731399841","GiftID":1}})";

        //std::string daoke = R"({"MsgID":1080876021,"Cmd":24,"MsgBody":{"Uin":441882,"RoleID":"425986807601037850","GiftID":1}})";


        std::string daoke = R"({"MsgID":943024804,"Cmd":22,"MsgBody":{"Uin":261281,"RoleID":"369691744731399841","GiftID":1}})";
        //std::string daoke = R"({"MsgID":943024804,"Cmd":3,"MsgBody":{"TargetPlayer":{"Uin":261281,"RoleID":"369691744731399841","RoleName":"晗月叶树"},"TargetType":1,"TextMsg":"sl 20"}})";
        buf->write_string(daoke);
        conn->flush();
    }

    void DnsClient::on_close(Connection *conn)
    {
        std::cout << "connection closed!\n";
    }

    bool DnsClient::connect(const std::string &ip, short port)
    {
        Socket *sock = new Socket(ip.c_str(), port, true);
        if (!sock->valid()) {
            std::cout << "cant create socket file descriptor!\n";
            delete sock;
            return false;
        }
        
        timer::WheelTimerManager manager;
        SelectPoller poller;
        net::EventLoop loop(&poller, &manager);
        timer_manager_ = &manager;
        ev_loop_ = &loop;

        UdpAcceptor acceptor(sock, &manager);
        if (!acceptor.listen()) {
            std::cout << "cant listen on port: " << port << "!\n";
            return false;
        }

        acceptor.set_event_handler(&loop);
        acceptor.set_connection_handler(this);

        auto instance = acceptor.get_udp_instance();
        const auto &res = instance->on_recv(*sock->get_address());
        if (!res.first || !res.second) {
            std::cout << "create udp connection failed!\n";
            return false;
        }

        UdpConnection *conn = static_cast<UdpConnection *>(res.second);
        conn->set_connection_handler(this);
        conn->set_event_handler(&loop);
        conn->set_instance_handler(instance);
        conn->set_connection_state(ConnectionState::connected); // 直接设置为已连接状态
        
        instance->enable_rw_events();
        loop.update_channel(conn->get_channel());

        loop.loop();

        return true;
    }
}