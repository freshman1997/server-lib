#include "dns_client.h"
#include "dns_packet.h"
#include "buffer/pool.h"
#include "event/event_loop.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/udp_connection.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/acceptor/udp_acceptor.h"
#include "net/connection/connection.h"
#include <iostream>

namespace yuan::net::dns
{
    DnsClient::DnsClient()
        : server_port_(53)
        , timer_manager_(nullptr)
        , poller_(nullptr)
        , ev_loop_(nullptr)
        , connection_(nullptr)
        , acceptor_(nullptr)
        , own_loop_(false)
        , next_session_id_(1)
        , got_response_(false)
        , timeout_timer_(nullptr)
    {
    }

    DnsClient::~DnsClient()
    {
        disconnect();
    }

    void DnsClient::on_connected(Connection *conn)
    {
        connection_ = conn;
        conn->set_connection_handler(this);
    }

    void DnsClient::on_error(Connection *conn)
    {
        std::cout << "DNS client connection error!\n";
        got_response_ = false;
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }
        if (own_loop_ && ev_loop_) {
            ev_loop_->quit();
        }
    }

    void DnsClient::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        if (buff && buff->readable_bytes() > 0) {
            handle_dns_response(conn, buff);
        }
    }

    void DnsClient::on_write(Connection *conn)
    {
    }

    void DnsClient::on_close(Connection *conn)
    {
        std::cout << "DNS client connection closed\n";
        connection_ = nullptr;
        got_response_ = false;
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }
        if (own_loop_ && ev_loop_) {
            ev_loop_->quit();
        }
    }

    void DnsClient::on_timer(timer::Timer *timer)
    {
        if (timer == timeout_timer_) {
            std::cout << "DNS query timed out\n";
            got_response_ = false;
            timeout_timer_ = nullptr;
            if (own_loop_ && ev_loop_) {
                ev_loop_->quit();
            }
        }
    }

    void DnsClient::handle_dns_response(Connection *conn, ::yuan::buffer::Buffer *buffer)
    {
        DnsPacket response;
        if (!response.deserialize(*buffer)) {
            std::cout << "Failed to parse DNS response\n";
            got_response_ = false;
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
            if (own_loop_ && ev_loop_) {
                ev_loop_->quit();
            }
            return;
        }

        std::cout << "DNS Response:\n" << response.to_string() << std::endl;

        last_response_ = std::move(response);
        got_response_ = true;

        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }

        if (response_handler_) {
            response_handler_(last_response_);
        }

        if (own_loop_ && ev_loop_) {
            ev_loop_->quit();
        }
    }

    bool DnsClient::connect(const std::string &ip, short port,
                            timer::TimerManager *timer_manager,
                            Poller *poller,
                            EventLoop *ev_loop)
    {
        server_ip_ = ip;
        server_port_ = port;
        own_loop_ = (timer_manager == nullptr || poller == nullptr || ev_loop == nullptr);

        if (own_loop_) {
            timer_manager_ = new timer::WheelTimerManager();
            poller_ = new net::SelectPoller();
            ev_loop_ = new net::EventLoop(poller_, timer_manager_);
        } else {
            timer_manager_ = timer_manager;
            poller_ = poller;
            ev_loop_ = ev_loop;
        }

        Socket *sock = new Socket(ip.c_str(), port, true);
        if (!sock->valid()) {
            std::cout << "DNS client: cannot create socket file descriptor!\n";
            if (own_loop_) {
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

        // Heap-allocate acceptor so it outlives connect() scope.
        // The acceptor owns the socket and udp instance; it must stay alive
        // until disconnect() is called.
        acceptor_ = new UdpAcceptor(sock, timer_manager_);
        if (!acceptor_->listen()) {
            std::cout << "DNS client: cannot listen!\n";
            delete acceptor_;
            acceptor_ = nullptr;
            if (own_loop_) {
                delete timer_manager_;
                delete poller_;
                delete ev_loop_;
                timer_manager_ = nullptr;
                poller_ = nullptr;
                ev_loop_ = nullptr;
            }
            return false;
        }

        acceptor_->set_event_handler(ev_loop_);
        acceptor_->set_connection_handler(static_cast<ConnectionHandler*>(this));

        auto instance = acceptor_->get_udp_instance();
        const auto &res = instance->on_recv(*sock->get_address());
        if (!res.first || !res.second) {
            std::cout << "DNS client: create udp connection failed!\n";
            delete acceptor_;
            acceptor_ = nullptr;
            if (own_loop_) {
                delete timer_manager_;
                delete poller_;
                delete ev_loop_;
                timer_manager_ = nullptr;
                poller_ = nullptr;
                ev_loop_ = nullptr;
            }
            return false;
        }

        UdpConnection *conn = static_cast<UdpConnection *>(res.second);
        conn->set_connection_handler(this);
        conn->set_event_handler(ev_loop_);
        conn->set_instance_handler(instance);
        conn->set_connection_state(ConnectionState::connected);

        instance->enable_rw_events();
        connection_ = conn;

        return true;
    }

    void DnsClient::disconnect()
    {
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }

        connection_ = nullptr;

        // Delete acceptor (which owns socket and udp instance).
        // Must be done before deleting ev_loop/poller/timer_manager.
        if (acceptor_) {
            delete acceptor_;
            acceptor_ = nullptr;
        }

        if (own_loop_) {
            if (ev_loop_) {
                delete ev_loop_;
                ev_loop_ = nullptr;
            }
            if (poller_) {
                delete poller_;
                poller_ = nullptr;
            }
            if (timer_manager_) {
                delete timer_manager_;
                timer_manager_ = nullptr;
            }
            own_loop_ = false;
        }
    }

    bool DnsClient::query(const std::string &domain, DnsType type, uint32_t timeout_ms)
    {
        return query(domain, type, nullptr, timeout_ms);
    }

    bool DnsClient::query(const std::string &domain, DnsType type, DnsResponseHandler handler, uint32_t timeout_ms)
    {
        if (!connection_) {
            std::cout << "DNS client: not connected\n";
            return false;
        }

        response_handler_ = handler;
        got_response_ = false;
        last_response_ = DnsPacket();

        uint16_t session_id = next_session_id_++;

        if (own_loop_) {
            // Set up timeout timer before entering the event loop
            if (timeout_timer_) {
                timeout_timer_->cancel();
            }
            timeout_timer_ = timer_manager_->timeout(timeout_ms, this);

            // Use queue_in_loop to send the packet inside the event loop,
            // then run the loop to actually perform I/O.
            // The loop exits when a response arrives or timeout fires.
            ev_loop_->queue_in_loop([this, domain, type, session_id]() {
                DnsPacket packet;
                packet.set_session_id(session_id);
                packet.set_is_response(false);
                packet.set_recursion_desired(true);

                DnsQuestion question;
                question.name = domain;
                question.type = type;
                question.class_ = DnsClass::IN;

                packet.add_question(question);

                auto buf = ::yuan::buffer::BufferedPool::get_instance()->allocate();
                packet.serialize(*buf);
                connection_->write_and_flush(buf);
            });

            ev_loop_->loop();

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
        } else {
            // External event loop: send directly, caller manages the loop
            if (timeout_ms > 0) {
                if (timeout_timer_) {
                    timeout_timer_->cancel();
                }
                timeout_timer_ = timer_manager_->timeout(timeout_ms, this);
            }

            DnsPacket packet;
            packet.set_session_id(session_id);
            packet.set_is_response(false);
            packet.set_recursion_desired(true);

            DnsQuestion question;
            question.name = domain;
            question.type = type;
            question.class_ = DnsClass::IN;

            packet.add_question(question);

            auto buf = ::yuan::buffer::BufferedPool::get_instance()->allocate();
            packet.serialize(*buf);
            connection_->write_and_flush(buf);
        }

        return got_response_;
    }

    const DnsPacket& DnsClient::get_last_response() const
    {
        return last_response_;
    }
}
