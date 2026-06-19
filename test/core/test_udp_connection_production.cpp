#include "buffer/byte_buffer.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/connection_factory.h"
#include "net/connection/datagram_transport.h"
#include "net/connection/udp_connection.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

#include <iostream>
#include <memory>
#include <string>

namespace
{
    int g_failed = 0;

    void check(bool condition, const char *message)
    {
        if (!condition) {
            ++g_failed;
            std::cerr << message << '\n';
        }
    }

    class FakeTimerManager final : public yuan::timer::TimerManager
    {
    public:
        void tick() override {}
        uint32_t get_time_unit() const override { return 10; }

    protected:
        yuan::timer::Timer *timeout(uint32_t, yuan::timer::TimerTask *) override { return nullptr; }
        yuan::timer::Timer *interval(uint32_t, uint32_t, yuan::timer::TimerTask *, int32_t) override { return nullptr; }
        bool schedule(yuan::timer::Timer *) override { return true; }
    };

    class FakeDatagramEndpoint final : public yuan::net::DatagramEndpoint
    {
    public:
        explicit FakeDatagramEndpoint(yuan::timer::TimerManager *timer_manager)
            : timer_manager_(timer_manager)
        {
        }

        int send_datagram(const std::shared_ptr<yuan::net::Connection> &conn, const yuan::buffer::ByteBuffer &buff) override
        {
            last_connection = conn;
            sent += std::string(buff.readable_span().begin(), buff.readable_span().end());
            return fail_send ? -1 : static_cast<int>(buff.readable_bytes());
        }

        int send_datagram(const yuan::net::InetAddress &addr, const yuan::buffer::ByteBuffer &buff) override
        {
            last_address = addr;
            sent += std::string(buff.readable_span().begin(), buff.readable_span().end());
            return fail_send ? -1 : static_cast<int>(buff.readable_bytes());
        }

        yuan::net::Channel *endpoint_channel() const override { return nullptr; }
        void update_endpoint_channel() override { ++updates; }
        yuan::timer::TimerManager *endpoint_timer_manager() const override { return timer_manager_; }

        bool fail_send = false;
        int updates = 0;
        std::string sent;
        yuan::net::InetAddress last_address;
        std::shared_ptr<yuan::net::Connection> last_connection;
        yuan::timer::TimerManager *timer_manager_ = nullptr;
    };

    class CountingHandler final : public yuan::net::ConnectionHandler
    {
    public:
        void on_connected(yuan::net::Connection &) override { ++connected; }
        void on_error(yuan::net::Connection &) override { ++errors; }
        void on_read(yuan::net::Connection &) override { ++reads; }
        void on_write(yuan::net::Connection &) override { ++writes; }
        void on_close(yuan::net::Connection &) override { ++closes; }

        int connected = 0;
        int errors = 0;
        int reads = 0;
        int writes = 0;
        int closes = 0;
    };

    yuan::buffer::ByteBuffer make_buffer(std::string_view text)
    {
        yuan::buffer::ByteBuffer buffer(text.size());
        buffer.append(text.data(), text.size());
        return buffer;
    }

    std::shared_ptr<yuan::net::UdpConnection> make_udp_connection(yuan::net::UdpInstance &instance,
                                                                  CountingHandler &handler,
                                                                  const yuan::net::InetAddress &peer)
    {
        auto conn = yuan::net::create_datagram_connection(peer);
        auto udp = std::dynamic_pointer_cast<yuan::net::UdpConnection>(conn);
        auto datagram = std::dynamic_pointer_cast<yuan::net::DatagramTransport>(conn);
        check(udp != nullptr && datagram != nullptr, "should create concrete UDP datagram connection");
        conn->set_connection_handler(yuan::net::make_non_owning_handler(handler));
        datagram->attach_datagram_instance(&instance);
        datagram->set_datagram_state(yuan::net::ConnectionState::connected);
        return udp;
    }

    void test_address_semantics_and_metrics()
    {
        FakeTimerManager timers;
        FakeDatagramEndpoint endpoint(&timers);
        yuan::net::UdpInstance instance(&endpoint);
        yuan::net::UdpConnectionOptions options;
        options.idle_check_interval_ms = 0;
        instance.set_options(options);

        const yuan::net::InetAddress peer("127.0.0.1", 53001);
        CountingHandler handler;
        auto udp = make_udp_connection(instance, handler, peer);

        check(udp->get_remote_address() == peer, "UDP remote address should be peer address");
        check(udp->peer_address() == peer, "UDP peer address should match remote address");
        check(udp->get_local_address() != peer, "UDP local address should not incorrectly alias peer address");

        udp->write_and_flush(make_buffer("hello"));
        auto metrics = instance.metrics();
        check(endpoint.sent == "hello", "UDP write_and_flush should send datagram");
        check(metrics.datagrams_written == 1 && metrics.bytes_written == 5, "UDP metrics should count sent datagrams/bytes");
        check(metrics.created_connections == 0, "manual attach should not count as UdpInstance-created connection");
        udp->abort();
    }

    void test_output_backpressure_drop_metrics()
    {
        FakeTimerManager timers;
        FakeDatagramEndpoint endpoint(&timers);
        yuan::net::UdpInstance instance(&endpoint);
        yuan::net::UdpConnectionOptions options;
        options.max_pending_output_bytes = 4;
        options.max_pending_output_datagrams = 1;
        options.idle_check_interval_ms = 0;
        instance.set_options(options);

        CountingHandler handler;
        auto udp = make_udp_connection(instance, handler, yuan::net::InetAddress("127.0.0.1", 53002));
        udp->write(make_buffer("abcd"));
        udp->write(make_buffer("ef"));

        check(udp->pending_output_bytes() == 4, "UDP pending bytes should respect configured limit");
        check(udp->pending_output_datagrams() == 1, "UDP pending datagrams should respect configured limit");
        check(udp->output_over_limit(), "UDP connection should expose output limit state");
        const auto metrics = instance.metrics();
        check(metrics.datagrams_dropped == 1 && metrics.bytes_dropped == 2, "UDP metrics should count dropped datagrams/bytes");
        udp->abort();
    }

    void test_send_error_metrics_and_close()
    {
        FakeTimerManager timers;
        FakeDatagramEndpoint endpoint(&timers);
        endpoint.fail_send = true;
        yuan::net::UdpInstance instance(&endpoint);
        yuan::net::UdpConnectionOptions options;
        options.idle_check_interval_ms = 0;
        instance.set_options(options);

        CountingHandler handler;
        auto udp = make_udp_connection(instance, handler, yuan::net::InetAddress("127.0.0.1", 53003));
        udp->write_and_flush(make_buffer("boom"));

        const auto metrics = instance.metrics();
        check(metrics.send_errors >= 1, "UDP metrics should count send errors");
        check(udp->get_connection_state() == yuan::net::ConnectionState::closed, "non-retryable send error should abort connection");
        check(handler.closes == 1, "send error abort should notify close once");
    }
}

int main()
{
    test_address_semantics_and_metrics();
    test_output_backpressure_drop_metrics();
    test_send_error_metrics_and_close();
    if (g_failed != 0) {
        return 1;
    }
    std::cout << "udp connection production tests passed\n";
    return 0;
}
