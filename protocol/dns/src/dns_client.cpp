#include "dns_client.h"
#include "dns_packet.h"
#include "buffer/byte_buffer.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/datagram_transport.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/timer_util.hpp"
#include "timer/wheel_timer_manager.h"
#include "net/connection/connection.h"
#include "logger.h"

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
        , coroutine_query_mode_(false)
        , timeout_timer_(nullptr)
    {
        completion_event_.reset();
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
        LOG_ERROR("DNS client connection error!");
        got_response_ = false;
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }
        notify_wait_completion();
    }

    void DnsClient::on_read(Connection *conn)
    {
        if (conn->get_input_byte_buffer().readable_bytes() > 0) {
            handle_dns_response(conn);
        }
    }

    void DnsClient::on_write(Connection *conn)
    {
    }

    void DnsClient::on_close(Connection *conn)
    {
        LOG_INFO("DNS client connection closed");
        connection_ = nullptr;
        got_response_ = false;
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }
        notify_wait_completion();
    }

    void DnsClient::on_timer(timer::Timer *timer)
    {
        if (timer == timeout_timer_) {
            LOG_WARN("DNS query timed out");
            got_response_ = false;
            timeout_timer_ = nullptr;
            notify_wait_completion();
        }
    }

    bool DnsClient::init_runtime(
        timer::TimerManager *timer_manager,
        Poller *poller,
        EventLoop *ev_loop)
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

    bool DnsClient::init_udp_connection()
    {
        Socket *sock = new Socket(server_ip_.c_str(), server_port_, true);
        if (!sock->valid()) {
            LOG_ERROR("DNS client: cannot create socket file descriptor!");
            delete sock;
            return false;
        }

        acceptor_ = create_datagram_acceptor(sock, timer_manager_);
        if (!acceptor_->listen()) {
            LOG_ERROR("DNS client: cannot listen!");
            delete acceptor_;
            acceptor_ = nullptr;
            return false;
        }

        acceptor_->set_event_handler(ev_loop_);
        acceptor_->set_connection_handler(static_cast<ConnectionHandler*>(this));
        ev_loop_->update_channel(acceptor_->endpoint_channel());

        auto instance = acceptor_->get_udp_instance();
        const auto &res = instance->on_recv(*sock->get_address());
        if (!res.first || !res.second) {
            LOG_ERROR("DNS client: create udp connection failed!");
            delete acceptor_;
            acceptor_ = nullptr;
            return false;
        }

        Connection *conn = res.second;
        auto *datagram = dynamic_cast<DatagramTransport *>(conn);
        if (!datagram) {
            delete acceptor_;
            acceptor_ = nullptr;
            return false;
        }

        conn->set_connection_handler(this);
        conn->set_event_handler(ev_loop_);
        datagram->attach_datagram_instance(instance);
        datagram->set_datagram_state(ConnectionState::connected);

        instance->enable_rw_events();
        connection_ = conn;
        return true;
    }

    void DnsClient::cleanup_runtime()
    {
        connection_ = nullptr;

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

    void DnsClient::notify_wait_completion()
    {
        if (!own_loop_ || !ev_loop_) {
            return;
        }

        if (coroutine_query_mode_) {
            completion_event_.notify();
            return;
        }

        ev_loop_->quit();
    }

    void DnsClient::handle_dns_response(Connection *conn)
    {
        DnsPacket response;
        auto byte_buffer = conn->take_input_byte_buffer();
        if (!response.deserialize(byte_buffer)) {
            LOG_WARN("Failed to parse DNS response");
            got_response_ = false;
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
            notify_wait_completion();
            return;
        }

        last_response_ = std::move(response);
        got_response_ = true;

        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }

        if (response_handler_) {
            response_handler_(last_response_);
        }

        notify_wait_completion();
    }

    void DnsClient::reset_query_state()
    {
        got_response_ = false;
        last_response_ = DnsPacket();
        completion_event_.reset(ev_loop_);
    }

    bool DnsClient::send_query_packet(const std::string &domain, DnsType type, uint16_t session_id)
    {
        if (!connection_) {
            return false;
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

        yuan::buffer::ByteBuffer byte_buffer;
        packet.serialize(byte_buffer);
        connection_->write_and_flush(byte_buffer);
        return true;
    }

    bool DnsClient::connect(const std::string &ip, short port,
                            timer::TimerManager *timer_manager,
                            Poller *poller,
                            EventLoop *ev_loop)
    {
        server_ip_ = ip;
        server_port_ = port;
        if (!init_runtime(timer_manager, poller, ev_loop)) {
            return false;
        }

        if (!init_udp_connection()) {
            cleanup_runtime();
            return false;
        }

        return true;
    }

    void DnsClient::disconnect()
    {
        if (timeout_timer_) {
            timeout_timer_->cancel();
            timeout_timer_ = nullptr;
        }

        cleanup_runtime();
    }

    bool DnsClient::query(const std::string &domain, DnsType type, uint32_t timeout_ms)
    {
        return query(domain, type, nullptr, timeout_ms);
    }

    bool DnsClient::query(const std::string &domain, DnsType type, DnsResponseHandler handler, uint32_t timeout_ms)
    {
        if (!connection_) {
            LOG_WARN("DNS client: not connected");
            return false;
        }

        response_handler_ = handler;
        reset_query_state();

        uint16_t session_id = next_session_id_++;

        if (own_loop_) {
            if (timeout_ms > 0) {
                timeout_timer_ = timer::TimerUtil::build_timeout_timer(
                    timer_manager_, timeout_ms, this, &DnsClient::on_timer);
            }

            if (!send_query_packet(domain, type, session_id)) {
                if (timeout_timer_) {
                    timeout_timer_->cancel();
                    timeout_timer_ = nullptr;
                }
                return false;
            }

            ev_loop_->loop();
        } else {
            // External event loop: send directly, caller manages the loop
            if (timeout_ms > 0) {
                if (timeout_timer_) {
                    timeout_timer_->cancel();
                }
                timeout_timer_ = timer::TimerUtil::build_timeout_timer(
                    timer_manager_, timeout_ms, this, &DnsClient::on_timer);
            }

            (void)send_query_packet(domain, type, session_id);
        }

        return got_response_;
    }

    yuan::coroutine::Task<DnsPacket> DnsClient::query_async(
        const std::string &domain,
        DnsType type,
        uint32_t timeout_ms)
    {
        if (!connection_) {
            co_return DnsPacket();
        }

        if (!query(domain, type, nullptr, timeout_ms)) {
            co_return DnsPacket();
        }

        co_return last_response_;
    }

    const DnsPacket& DnsClient::get_last_response() const
    {
        return last_response_;
    }
}

