#include "base/time.h"
#include "net/acceptor/udp/kcp_adapter.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

#include <iostream>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    class FakeTimer final : public yuan::timer::Timer
    {
    public:
        FakeTimer(yuan::timer::TimerTask *task, uint32_t timeout, uint32_t interval, int32_t period)
            : task_(task), timeout_(timeout), interval_(interval), period_(period), handle_state_(std::make_shared<yuan::timer::TimerHandleState>())
        {
            handle_state_->bind([this]() {
                cancelled_ = true;
                handle_state_->clear();
            });
        }

        bool ready() const override
        {
            return !cancelled_;
        }

        void cancel() override
        {
            cancelled_ = true;
            if (handle_state_) {
                handle_state_->clear();
            }
        }

        void reset() override
        {
            cancelled_ = false;
        }

        bool is_processing() const override
        {
            return false;
        }

        bool is_done() const override
        {
            return false;
        }

        bool is_cancel() const override
        {
            return cancelled_;
        }

        yuan::timer::TimerTask *get_task() const override
        {
            return task_;
        }

        std::shared_ptr<yuan::timer::TimerHandleState> handle_state() const override
        {
            return handle_state_;
        }

        uint32_t timeout() const
        {
            return timeout_;
        }

        uint32_t interval() const
        {
            return interval_;
        }

        int32_t period() const
        {
            return period_;
        }

    private:
        yuan::timer::TimerTask *task_ = nullptr;
        uint32_t timeout_ = 0;
        uint32_t interval_ = 0;
        int32_t period_ = 0;
        bool cancelled_ = false;
        std::shared_ptr<yuan::timer::TimerHandleState> handle_state_;
    };

    class FakeTimerManager final : public yuan::timer::TimerManager
    {
    public:
        void tick() override
        {
        }

        uint32_t get_time_unit() const override
        {
            return 10;
        }

        const FakeTimer *last_timer() const
        {
            return timers_.empty() ? nullptr : timers_.back().get();
        }

        std::shared_ptr<yuan::timer::TimerHandleState> last_timer_handle_state() const
        {
            const auto *timer = last_timer();
            return timer ? timer->handle_state() : nullptr;
        }

    protected:
        yuan::timer::Timer *timeout(uint32_t milliseconds, yuan::timer::TimerTask *task) override
        {
            timers_.push_back(std::make_unique<FakeTimer>(task, milliseconds, milliseconds, 1));
            return timers_.back().get();
        }

        yuan::timer::Timer *interval(uint32_t timeout, uint32_t interval, yuan::timer::TimerTask *task, int32_t period) override
        {
            timers_.push_back(std::make_unique<FakeTimer>(task, timeout, interval, period));
            return timers_.back().get();
        }

        bool schedule(yuan::timer::Timer *timer) override
        {
            return timer != nullptr;
        }

    private:
        std::vector<std::unique_ptr<FakeTimer>> timers_;
    };

    class FakeConnection final : public yuan::net::Connection
    {
    public:
        yuan::net::ConnectionState get_connection_state() const override
        {
            return yuan::net::ConnectionState::connected;
        }

        bool is_connected() const override
        {
            return true;
        }

        const yuan::net::InetAddress &get_remote_address() const override
        {
            return remote_;
        }

        const yuan::net::InetAddress &get_local_address() const override
        {
            return local_;
        }

        void write(const yuan::buffer::ByteBuffer &) override
        {
        }

        void write_and_flush(const yuan::buffer::ByteBuffer &) override
        {
        }

        void flush() override
        {
            ++flush_count;

            while (!output_buffer_.empty()) {
                auto packet = output_buffer_.pop_front();
                if (!packet || packet->empty() || peer_connection == nullptr) {
                    continue;
                }

                peer_connection->pending_datagrams.push_back(packet->copy_readable());
            }
        }

        void drain_pending_datagrams()
        {
            if (peer_adapter == nullptr) {
                pending_datagrams.clear();
                return;
            }

            for (auto &packet : pending_datagrams) {
                yuan::buffer::ByteBuffer recv = packet.copy_readable();
                const int decoded = peer_adapter->on_recv(recv);
                if (decoded > 0) {
                    received_payloads.emplace_back(recv.read_ptr(), static_cast<std::size_t>(decoded));
                } else if (decoded < 0) {
                    recv_errors.push_back(decoded);
                }
            }
            pending_datagrams.clear();
        }

        void abort() override
        {
        }

        void close() override
        {
        }

        void set_connection_handler(std::shared_ptr<yuan::net::ConnectionHandler>) override
        {
        }

        yuan::net::ConnectionHandler *get_connection_handler() const override
        {
            return nullptr;
        }

        void set_ssl_handler(std::shared_ptr<yuan::net::SSLHandler>) override
        {
        }

        void on_read_event() override
        {
        }

        void on_write_event() override
        {
        }

        void set_event_handler(yuan::net::EventHandler *) override
        {
        }

        yuan::net::KcpAdapter *peer_adapter = nullptr;
        FakeConnection *peer_connection = nullptr;
        int flush_count = 0;
        std::vector<std::string> received_payloads;
        std::vector<int> recv_errors;
        std::vector<yuan::buffer::ByteBuffer> pending_datagrams;

    private:
        yuan::net::InetAddress remote_{"127.0.0.1", 9000};
        yuan::net::InetAddress local_{"127.0.0.1", 9001};
    };

    bool expect(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    bool test_init_registers_periodic_timer()
    {
        FakeTimerManager timer_manager;
        FakeConnection conn;
        yuan::net::KcpAdapter adapter;
        if (!expect(adapter.init(&conn, &timer_manager), "kcp adapter init failed")) {
            return false;
        }

        const FakeTimer *timer = timer_manager.last_timer();
        if (!expect(timer != nullptr, "kcp adapter should register update timer")) {
            return false;
        }

        return expect(timer->timeout() == 0 && timer->interval() == timer_manager.get_time_unit() && timer->period() == -1,
                      "kcp adapter timer registration uses unexpected timeout/interval/period");
    }

    bool test_timer_handle_released_on_adapter_destruction()
    {
        FakeTimerManager timer_manager;
        FakeConnection conn;
        std::shared_ptr<yuan::timer::TimerHandleState> handle_state;

        {
            yuan::net::KcpAdapter adapter;
            if (!expect(adapter.init(&conn, &timer_manager), "kcp adapter init failed")) {
                return false;
            }

            handle_state = timer_manager.last_timer_handle_state();
            if (!expect(handle_state && handle_state->active(), "kcp adapter timer handle should be active after init")) {
                return false;
            }
        }

        return expect(handle_state && !handle_state->active(), "kcp adapter timer handle should be canceled on destruction");
    }

    bool test_rejects_invalid_kcp_packet()
    {
        FakeTimerManager timer_manager;
        FakeConnection conn;
        yuan::net::KcpAdapter adapter;
        if (!expect(adapter.init(&conn, &timer_manager), "kcp adapter init failed")) {
            return false;
        }

        yuan::buffer::ByteBuffer invalid_packet(std::string_view("not-a-kcp-frame"));
        const int ret = adapter.on_recv(invalid_packet);
        return expect(ret < 0, "invalid kcp packet should return error");
    }

    bool test_bidirectional_delivery_after_timer_update()
    {
        FakeTimerManager timer_manager;
        FakeConnection sender_conn;
        FakeConnection receiver_conn;
        yuan::net::KcpAdapter sender;
        yuan::net::KcpAdapter receiver;

        if (!expect(sender.init(&sender_conn, &timer_manager), "sender kcp adapter init failed") ||
            !expect(receiver.init(&receiver_conn, &timer_manager), "receiver kcp adapter init failed")) {
            return false;
        }

        sender_conn.peer_adapter = &receiver;
        sender_conn.peer_connection = &receiver_conn;
        receiver_conn.peer_adapter = &sender;
        receiver_conn.peer_connection = &sender_conn;

        const std::string payload = "hello-kcp-adapter";
        yuan::buffer::ByteBuffer write_buffer{std::string_view(payload)};
        if (!expect(sender.on_write(write_buffer) == static_cast<int>(payload.size()), "kcp send should accept full payload")) {
            return false;
        }

        for (int i = 0; i < 100 && receiver_conn.received_payloads.empty(); ++i) {
            yuan::base::time::advance_steady_time_for_test(10);
            sender.on_timer(nullptr);
            receiver.on_timer(nullptr);
            receiver_conn.drain_pending_datagrams();
            sender_conn.drain_pending_datagrams();
        }

        if (!expect(sender_conn.flush_count > 0, "kcp send path should flush datagrams")) {
            return false;
        }

        if (!expect(!receiver_conn.received_payloads.empty(), "receiver should decode kcp payload")) {
            return false;
        }

        return expect(receiver_conn.received_payloads.front() == payload, "receiver payload should match sender payload");
    }

    bool test_large_payload_decode_buffer_overflow_path()
    {
        FakeTimerManager timer_manager;
        FakeConnection sender_conn;
        FakeConnection receiver_conn;
        yuan::net::KcpAdapter sender;
        yuan::net::KcpAdapter receiver;

        if (!expect(sender.init(&sender_conn, &timer_manager), "sender kcp adapter init failed") ||
            !expect(receiver.init(&receiver_conn, &timer_manager), "receiver kcp adapter init failed")) {
            return false;
        }

        sender_conn.peer_adapter = &receiver;
        sender_conn.peer_connection = &receiver_conn;
        receiver_conn.peer_adapter = &sender;
        receiver_conn.peer_connection = &sender_conn;

        const std::string large_payload(2000, 'x');
        yuan::buffer::ByteBuffer write_buffer{std::string_view(large_payload)};
        if (!expect(sender.on_write(write_buffer) == static_cast<int>(large_payload.size()), "kcp should enqueue large payload")) {
            return false;
        }

        for (int i = 0; i < 200; ++i) {
            yuan::base::time::advance_steady_time_for_test(10);
            sender.on_timer(nullptr);
            receiver.on_timer(nullptr);
            receiver_conn.drain_pending_datagrams();
            sender_conn.drain_pending_datagrams();
        }

        bool has_small_decode_buffer_error = false;
        for (int error : receiver_conn.recv_errors) {
            if (error == -3) {
                has_small_decode_buffer_error = true;
                break;
            }
        }

        return expect(has_small_decode_buffer_error,
                      "receiver should report IKCP_EBUF(-3) when decoded payload exceeds adapter buffer");
    }
}

int main()
{
    yuan::base::time::set_steady_time_for_test(1000);

    const bool ok = test_init_registers_periodic_timer()
        && test_timer_handle_released_on_adapter_destruction()
        && test_rejects_invalid_kcp_packet()
        && test_bidirectional_delivery_after_timer_update()
        && test_large_payload_decode_buffer_overflow_path();

    yuan::base::time::reset_test_time();
    if (!ok) {
        return 1;
    }

    std::cout << "udp kcp adapter tests passed\n";
    return 0;
}
