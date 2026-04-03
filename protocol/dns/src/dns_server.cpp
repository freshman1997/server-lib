#include "dns_server.h"
#include "dns_packet.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/connection/connection.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "buffer/pool.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace yuan::net::dns
{
    DnsServer::DnsServer()
        : port_(53)
        , running_(false)
        , timer_manager_(nullptr)
        , poller_(nullptr)
        , ev_loop_(nullptr)
    {
        add_record("localhost", "127.0.0.1");
    }

    DnsServer::~DnsServer()
    {
        stop();
    }

    void DnsServer::on_connected(Connection *conn)
    {
        conn->set_connection_handler(this);
    }

    void DnsServer::on_error(Connection *conn)
    {
        std::cout << "DNS server connection error!\n";
    }

    void DnsServer::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        if (buff && buff->readable_bytes() > 0) {
            handle_dns_query(conn, buff);
        }
    }

    void DnsServer::on_write(Connection *conn)
    {
    }

    void DnsServer::on_close(Connection *conn)
    {
        std::cout << "DNS server connection closed\n";
    }

    void DnsServer::handle_dns_query(Connection *conn, ::yuan::buffer::Buffer *buffer)
    {
        DnsPacket query;
        if (!query.deserialize(*buffer)) {
            std::cout << "Failed to parse DNS query\n";
            return;
        }

        std::cout << query.to_string() << std::endl;

        DnsPacket response;
        create_response(query, response);

        auto response_buffer = ::yuan::buffer::BufferedPool::get_instance()->allocate();
        response.serialize(*response_buffer);
        conn->write_and_flush(response_buffer);
    }

    void DnsServer::create_response(const DnsPacket &query, DnsPacket &response)
    {
        response.set_session_id(query.get_session_id());
        response.set_is_response(true);
        response.set_opcode(query.get_opcode());
        response.set_recursion_desired(query.is_recursion_desired());
        response.set_recursion_available(true);
        response.set_authoritative_answer(true);

        for (const auto &question : query.get_questions()) {
            response.add_question(question);

            if (query_handler_) {
                query_handler_(query, response);
            }

            if (response.get_answers().empty()) {
                DnsResourceRecord record = find_record(question.name, question.type);
                if (record.name.empty()) {
                    response.set_response_code(DnsResponseCode::NAME_ERROR);
                } else {
                    response.add_answer(record);
                }
            }
        }

        if (response.get_answers().empty() && response.get_response_code() == DnsResponseCode::NO_ERROR) {
            response.set_response_code(DnsResponseCode::NAME_ERROR);
        }
    }

    DnsResourceRecord DnsServer::find_record(const std::string &name, DnsType type)
    {
        auto key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        auto it = dns_records_.find(key);
        if (it != dns_records_.end()) {
            if (it->second.type == type || type == DnsType::ANY) {
                return it->second;
            }
        }

        return DnsResourceRecord();
    }

    bool DnsServer::serve(int port)
    {
        return serve(port, nullptr, nullptr, nullptr);
    }

    bool DnsServer::serve(int port, timer::TimerManager *timer_manager, Poller *poller, EventLoop *ev_loop)
    {
        port_ = port;
        bool own_loop = (timer_manager == nullptr || poller == nullptr || ev_loop == nullptr);

        if (own_loop) {
            timer_manager_ = new timer::WheelTimerManager();
            poller_ = new net::SelectPoller();
            ev_loop_ = new net::EventLoop(poller_, timer_manager_);
        } else {
            timer_manager_ = timer_manager;
            poller_ = poller;
            ev_loop_ = ev_loop;
        }

        Socket *sock = new Socket("", port, true);
        if (!sock->valid()) {
            std::cout << "DNS server: cannot create socket file descriptor!\n";
            if (own_loop) {
                delete timer_manager_;
                delete poller_;
                delete ev_loop_;
                timer_manager_ = nullptr;
                poller_ = nullptr;
                ev_loop_ = nullptr;
            }
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            std::cout << "DNS server: cannot bind port: " << port << "!\n";
            if (own_loop) {
                delete timer_manager_;
                delete poller_;
                delete ev_loop_;
                timer_manager_ = nullptr;
                poller_ = nullptr;
                ev_loop_ = nullptr;
            }
            delete sock;
            return false;
        }

        sock->set_no_delay(true);
        sock->set_reuse(true);
        sock->set_none_block(true);

        UdpAcceptor *acceptor = new UdpAcceptor(sock, timer_manager_);
        if (!acceptor->listen()) {
            std::cout << "DNS server: cannot listen on port: " << port << "!\n";
            delete acceptor;
            if (own_loop) {
                delete timer_manager_;
                delete poller_;
                delete ev_loop_;
                timer_manager_ = nullptr;
                poller_ = nullptr;
                ev_loop_ = nullptr;
            }
            delete sock;
            return false;
        }

        acceptor->set_event_handler(ev_loop_);
        acceptor->set_connection_handler(static_cast<ConnectionHandler*>(this));

        running_ = true;
        ev_loop_->loop();

        running_ = false;
        delete acceptor;

        if (own_loop) {
            delete timer_manager_;
            delete poller_;
            delete ev_loop_;
            timer_manager_ = nullptr;
            poller_ = nullptr;
            ev_loop_ = nullptr;
        }

        return true;
    }

    void DnsServer::stop()
    {
        running_ = false;
        if (ev_loop_) {
            ev_loop_->quit();
        }
    }

    void DnsServer::set_query_handler(DnsQueryHandler handler)
    {
        query_handler_ = handler;
    }

    void DnsServer::add_record(const std::string &name, const std::string &ip, DnsType type)
    {
        DnsResourceRecord record;
        record.name = name;
        record.type = type;
        record.class_ = DnsClass::IN;
        record.ttl = 3600;
        record.set_rdata_from_string(ip);

        auto key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        dns_records_[key] = record;
    }
}
