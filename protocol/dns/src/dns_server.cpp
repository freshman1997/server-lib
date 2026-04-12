#include "dns_server.h"
#include "dns_packet.h"
#include "buffer/byte_buffer.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/connection/connection.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "logger.h"
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
        , acceptor_(nullptr)
        , own_loop_(false)
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
        LOG_ERROR("DNS server connection error!");
    }

    void DnsServer::on_read(Connection *conn)
    {
        auto byte_buffer = conn->get_input_byte_buffer();
        if (byte_buffer.readable_bytes() > 0) {
            handle_dns_query(conn, byte_buffer);
        }
    }

    void DnsServer::on_write(Connection *conn)
    {
    }

    void DnsServer::on_close(Connection *conn)
    {
        LOG_INFO("DNS server connection closed");
    }

    void DnsServer::handle_dns_query(Connection *conn, const ::yuan::buffer::ByteBuffer &buffer)
    {
        DnsPacket query;
        auto byte_buffer = buffer;
        if (!query.deserialize(byte_buffer)) {
            LOG_WARN("Failed to parse DNS query");
            return;
        }

        DnsPacket response;
        create_response(query, response);

        yuan::buffer::ByteBuffer response_buffer;
        response.serialize(response_buffer);
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

    bool DnsServer::init_runtime(timer::TimerManager *timer_manager, Poller *poller, EventLoop *ev_loop)
    {
        own_loop_ = (timer_manager == nullptr || poller == nullptr || ev_loop == nullptr);

        if (own_loop_) {
            timer_manager_ = new timer::WheelTimerManager();
            poller_ = new net::SelectPoller();
            ev_loop_ = new net::EventLoop(poller_, timer_manager_);
            return timer_manager_ && poller_ && ev_loop_;
        }

        timer_manager_ = timer_manager;
        poller_ = poller;
        ev_loop_ = ev_loop;
        return timer_manager_ && poller_ && ev_loop_;
    }

    bool DnsServer::init_udp_server()
    {
        Socket *sock = new Socket("", port_, true);
        if (!sock->valid()) {
            LOG_ERROR("DNS server: cannot create socket file descriptor!");
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            LOG_ERROR("DNS server: cannot bind port: {}!", port_);
            delete sock;
            return false;
        }

        sock->set_no_delay(true);
        sock->set_reuse(true);
        sock->set_none_block(true);

        acceptor_ = create_datagram_acceptor(sock, timer_manager_);
        if (!acceptor_->listen()) {
            LOG_ERROR("DNS server: cannot listen on port: {}!", port_);
            delete acceptor_;
            acceptor_ = nullptr;
            delete sock;
            return false;
        }

        acceptor_->set_event_handler(ev_loop_);
        acceptor_->set_connection_handler(static_cast<ConnectionHandler*>(this));
        ev_loop_->update_channel(acceptor_->endpoint_channel());
        return true;
    }

    void DnsServer::cleanup_runtime()
    {
        if (acceptor_) {
            delete acceptor_;
            acceptor_ = nullptr;
        }

        if (own_loop_) {
            if (ev_loop_) {
                delete ev_loop_;
            }
            if (poller_) {
                delete poller_;
            }
            if (timer_manager_) {
                delete timer_manager_;
            }
        }

        ev_loop_ = nullptr;
        poller_ = nullptr;
        timer_manager_ = nullptr;
        own_loop_ = false;
    }

    bool DnsServer::serve(int port)
    {
        return serve(port, nullptr, nullptr, nullptr);
    }

    bool DnsServer::serve(int port, timer::TimerManager *timer_manager, Poller *poller, EventLoop *ev_loop)
    {
        port_ = port;
        if (!init_runtime(timer_manager, poller, ev_loop)) {
            return false;
        }

        if (!init_udp_server()) {
            cleanup_runtime();
            return false;
        }

        running_ = true;
        ev_loop_->loop();

        running_ = false;
        cleanup_runtime();

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
