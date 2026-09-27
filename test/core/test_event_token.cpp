#include "event/event_loop.h"
#include "net/channel/channel.h"
#include "net/handler/event_handler.h"
#include "net/handler/select_handler.h"
#include "net/poller/poll_poller.h"
#include "net/poller/poller.h"
#include "timer/wheel_timer_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#endif

namespace
{
    int g_passed = 0;
    int g_failed = 0;

    void check(bool condition, const std::string &message)
    {
        if (condition) {
            ++g_passed;
        } else {
            std::cerr << "  FAIL: " << message << "\n";
            ++g_failed;
        }
    }

    class NoopPoller final : public yuan::net::Poller
    {
    public:
        bool init() override
        {
            return true;
        }

        uint64_t poll(uint32_t, std::vector<yuan::net::PollEvent> &) override
        {
            return 0;
        }

        void update_channel(yuan::net::Channel *channel) override
        {
            updated = channel;
            ++update_count;
        }

        void remove_channel(yuan::net::Channel *channel) override
        {
            removed = channel;
            ++remove_count;
        }

        yuan::net::Channel *updated = nullptr;
        yuan::net::Channel *removed = nullptr;
        int update_count = 0;
        int remove_count = 0;
    };

    class ScriptedPoller final : public yuan::net::Poller
    {
    public:
        bool init() override
        {
            return true;
        }

        uint64_t poll(uint32_t, std::vector<yuan::net::PollEvent> &events) override
        {
            ++poll_count;
            std::vector<yuan::net::PollEvent> next_events;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                next_events.swap(scripted_events_);
            }
            events.insert(events.end(), next_events.begin(), next_events.end());
            return events.size();
        }

        void update_channel(yuan::net::Channel *channel) override
        {
            updated = channel;
            ++update_count;
        }

        void remove_channel(yuan::net::Channel *channel) override
        {
            removed = channel;
            ++remove_count;
        }

        void push_event(const yuan::net::PollEvent &event)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scripted_events_.push_back(event);
        }

        yuan::net::Channel *updated = nullptr;
        yuan::net::Channel *removed = nullptr;
        int update_count = 0;
        int remove_count = 0;
        int poll_count = 0;

    private:
        std::mutex mutex_;
        std::vector<yuan::net::PollEvent> scripted_events_;
    };

    class CountingHandler final : public yuan::net::SelectHandler
    {
    public:
        explicit CountingHandler(yuan::net::Channel *channel = nullptr)
            : channel_(channel)
        {
        }

        void on_read_event() override
        {
            ++read_count;
            if (close_on_read && event_handler_ && channel_) {
                event_handler_->close_channel(channel_);
            }
        }

        void on_write_event() override
        {
            ++write_count;
        }

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

        int read_count = 0;
        int write_count = 0;
        bool close_on_read = false;

    private:
        yuan::net::Channel *channel_ = nullptr;
        yuan::net::EventHandler *event_handler_ = nullptr;
    };

#ifndef _WIN32
    class FdGuard
    {
    public:
        explicit FdGuard(int fd = -1) : fd_(fd) {}
        ~FdGuard()
        {
            if (fd_ >= 0) {
                ::close(fd_);
            }
        }
        FdGuard(const FdGuard &) = delete;
        FdGuard &operator=(const FdGuard &) = delete;
        int get() const { return fd_; }
        int release()
        {
            const int fd = fd_;
            fd_ = -1;
            return fd;
        }

    private:
        int fd_;
    };

    class PipeReadHandler final : public yuan::net::SelectHandler
    {
    public:
        PipeReadHandler(int fd, yuan::net::Channel *channel)
            : fd_(fd), channel_(channel)
        {
        }

        void on_read_event() override
        {
            ++read_count;
            char buf[32];
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            (void)n;
            if (close_on_read && event_handler_ && channel_) {
                event_handler_->close_channel(channel_);
            }
            if (quit_on_read && event_handler_) {
                event_handler_->quit();
            }
        }

        void on_write_event() override {}

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

        int read_count = 0;
        bool close_on_read = false;
        bool quit_on_read = false;

    private:
        int fd_;
        yuan::net::Channel *channel_;
        yuan::net::EventHandler *event_handler_ = nullptr;
    };

    class RegistrationPipeHandler final : public yuan::net::SelectHandler
    {
    public:
        explicit RegistrationPipeHandler(int fd) : fd_(fd) {}

        void on_read_event() override
        {
            ++read_count;
            char buf[32];
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            (void)n;
            if (on_read) {
                on_read();
            }
            if (quit_on_read && event_handler_) {
                event_handler_->quit();
            }
        }

        void on_write_event() override {}

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

        int read_count = 0;
        bool quit_on_read = false;
        std::function<void()> on_read;

    private:
        int fd_;
        yuan::net::EventHandler *event_handler_ = nullptr;
    };

    bool make_pipe(int fds[2])
    {
        if (::pipe(fds) != 0) {
            return false;
        }
        (void)::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
        (void)::fcntl(fds[1], F_SETFL, ::fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
        return true;
    }
#endif

    class UdpSelfSocket
    {
    public:
        UdpSelfSocket() = default;
        ~UdpSelfSocket() { close(); }
        UdpSelfSocket(const UdpSelfSocket &) = delete;
        UdpSelfSocket &operator=(const UdpSelfSocket &) = delete;

        bool init()
        {
#ifdef _WIN32
            const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                return false;
            }
            fd_ = static_cast<int>(s);
            u_long non_blocking = 1;
            (void)::ioctlsocket(s, FIONBIO, &non_blocking);
#else
            const int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s < 0) {
                return false;
            }
            fd_ = s;
            const int flags = ::fcntl(s, F_GETFL, 0);
            (void)::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
            addr_.sin_family = AF_INET;
            addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr_.sin_port = 0;
            if (::bind(native(), reinterpret_cast<sockaddr *>(&addr_), sizeof(addr_)) != 0) {
                close();
                return false;
            }
#ifdef _WIN32
            int addr_len = static_cast<int>(sizeof(addr_));
#else
            socklen_t addr_len = sizeof(addr_);
#endif
            if (::getsockname(native(), reinterpret_cast<sockaddr *>(&addr_), &addr_len) != 0) {
                close();
                return false;
            }
            return true;
        }

        int fd() const { return fd_; }

        void notify()
        {
            if (fd_ < 0) {
                return;
            }
            const char byte = 'u';
            (void)::sendto(native(), &byte, 1, 0, reinterpret_cast<sockaddr *>(&addr_), sizeof(addr_));
        }

        void drain()
        {
            if (fd_ < 0) {
                return;
            }
            char buf[64];
            while (::recv(native(), buf, sizeof(buf), 0) > 0) {
            }
        }

        void close()
        {
            if (fd_ < 0) {
                return;
            }
#ifdef _WIN32
            ::closesocket(native());
#else
            ::close(fd_);
#endif
            fd_ = -1;
        }

    private:
#ifdef _WIN32
        SOCKET native() const { return static_cast<SOCKET>(fd_); }
#else
        int native() const { return fd_; }
#endif

        int fd_ = -1;
        sockaddr_in addr_{};
    };

    class UdpReadHandler final : public yuan::net::SelectHandler
    {
    public:
        explicit UdpReadHandler(UdpSelfSocket *sock)
            : sock_(sock)
        {
        }

        void on_read_event() override
        {
            ++read_count;
            sock_->drain();
            if (quit_on_read && event_handler_) {
                event_handler_->quit();
            }
        }

        void on_write_event() override {}

        void set_event_handler(yuan::net::EventHandler *eventHandler) override
        {
            event_handler_ = eventHandler;
        }

        int read_count = 0;
        bool quit_on_read = false;

    private:
        UdpSelfSocket *sock_;
        yuan::net::EventHandler *event_handler_ = nullptr;
    };

    void test_generation_validation()
    {
        std::cout << "  [EventToken] generation validation\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        yuan::net::Channel channel(42);
        channel.enable_read();

        loop.update_channel(&channel);
        const auto generation = channel.generation();

        yuan::net::PollEvent valid{42, yuan::net::Channel::READ_EVENT, generation};
        yuan::net::PollEvent zero_generation{42, yuan::net::Channel::READ_EVENT, 0};
        yuan::net::PollEvent stale{42, yuan::net::Channel::READ_EVENT, generation + 1};

        check(loop.accepts_poll_event_for_test(valid), "matching generation should be accepted");
        check(!loop.accepts_poll_event_for_test(zero_generation), "zero generation must not bypass validation");
        check(!loop.accepts_poll_event_for_test(stale), "stale generation should be rejected");

        loop.close_channel(&channel);
        check(channel.generation() != generation, "close_channel should invalidate old generation");
        check(!loop.accepts_poll_event_for_test(valid), "old event should be rejected after close_channel");
    }

    void test_reused_fd_stale_channel_is_ignored()
    {
        std::cout << "  [EventToken] reused fd ignores stale channel cleanup\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        yuan::net::Channel old_channel(77);
        old_channel.enable_read();
        loop.update_channel(&old_channel);
        const auto old_generation = old_channel.generation();

        yuan::net::Channel new_channel(77);
        new_channel.enable_read();
        loop.update_channel(&new_channel);
        const auto new_generation = new_channel.generation();

        check(new_generation != old_generation, "reused fd should get a new generation");

        yuan::net::PollEvent old_event{77, yuan::net::Channel::READ_EVENT, old_generation};
        yuan::net::PollEvent new_event{77, yuan::net::Channel::READ_EVENT, new_generation};
        check(!loop.accepts_poll_event_for_test(old_event), "old event should not match reused fd");
        check(loop.accepts_poll_event_for_test(new_event), "new event should match reused fd");

        const int remove_count_before_stale_close = poller.remove_count;
        loop.close_channel(&old_channel);
        check(poller.remove_count == remove_count_before_stale_close,
              "stale close_channel should not remove the active reused fd channel");
        check(loop.accepts_poll_event_for_test(new_event), "stale close should leave new channel registered");

        old_channel.disable_all();
        loop.update_channel(&old_channel);
        check(loop.accepts_poll_event_for_test(new_event), "stale empty update should leave new channel registered");

        loop.close_channel(&new_channel);
        check(!loop.accepts_poll_event_for_test(new_event), "active reused fd channel should close normally");
    }

    void test_callback_close_invalidates_later_events()
    {
        std::cout << "  [EventToken] callback close invalidates later events\n";

        ScriptedPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        yuan::net::Channel channel(88);
        CountingHandler handler(&channel);
        handler.close_on_read = true;
        handler.set_event_handler(&loop);
        channel.set_handler(&handler);
        channel.enable_read();
        loop.update_channel(&channel);
        const auto generation = channel.generation();

        poller.push_event({88, yuan::net::Channel::READ_EVENT, generation});
        poller.push_event({88, yuan::net::Channel::READ_EVENT, generation});
        loop.queue_in_loop([&loop]() { loop.quit(); });
        (void)loop.loop();

        check(handler.read_count == 1, "close during callback should prevent later same-generation event dispatch");
        check(poller.remove_count == 1, "callback close should unregister exactly once");
        check(!loop.accepts_poll_event_for_test({88, yuan::net::Channel::READ_EVENT, generation}),
              "callback close should invalidate old poll events");
    }

    void test_cross_thread_queue_wakes_idle_loop()
    {
        std::cout << "  [EventToken] cross-thread queue wakes idle loop\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        std::mutex mutex;
        std::condition_variable cv;
        bool callback_ran = false;

        std::thread loop_thread([&loop]() {
            (void)loop.loop();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        loop.queue_in_loop([&]() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_ran = true;
            }
            cv.notify_one();
            loop.quit();
        });

        {
            std::unique_lock<std::mutex> lock(mutex);
            check(cv.wait_for(lock, std::chrono::seconds(1), [&]() { return callback_ran; }),
                  "queued callback should run without registered channels");
        }

        loop_thread.join();
    }

    void test_quit_before_loop_is_preserved()
    {
        std::cout << "  [EventToken] quit before loop is preserved\n";

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        loop.quit();

        const auto started = std::chrono::steady_clock::now();
        const auto reason = loop.loop();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started).count();
        check(reason == yuan::net::EventLoopExitReason::quit_requested,
              "pre-start quit should remain visible to loop");
        check(elapsed_ms < 100, "pre-start quit should return without polling");
    }

    void test_cross_thread_sync_operation_and_close()
    {
        std::cout << "  [EventToken] cross-thread synchronous channel operation\n";

        yuan::net::PollPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        UdpSelfSocket sock;
        check(sock.init(), "udp self socket should initialize for sync operation test");

        yuan::net::Channel channel(sock.fd());
        channel.enable_read();
        std::thread loop_thread([&loop]() { (void)loop.loop(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        loop.update_channel(&channel);
        const auto generation = channel.generation();
        loop.close_channel(&channel);
        loop.quit();
        loop_thread.join();

        check(generation != 0, "synchronous cross-thread update should assign generation");
        check(channel.generation() != generation,
              "synchronous cross-thread close should complete before returning");
    }

    void test_real_fd_read_close_lifecycle()
    {
        std::cout << "  [EventToken] real fd read close lifecycle\n";
#ifdef _WIN32
        check(true, "real fd lifecycle test skipped on Windows");
#else
        int fds[2] = {-1, -1};
        if (!make_pipe(fds)) {
            check(false, "pipe should be available for real fd lifecycle test");
            return;
        }
        FdGuard read_fd(fds[0]);
        FdGuard write_fd(fds[1]);

        yuan::net::PollPoller poller;
        check(poller.init(), "poll poller should initialize");
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        yuan::net::Channel channel(read_fd.get());
        PipeReadHandler handler(read_fd.get(), &channel);
        handler.close_on_read = true;
        handler.quit_on_read = true;
        handler.set_event_handler(&loop);
        channel.set_handler(&handler);
        channel.enable_read();
        loop.update_channel(&channel);
        const auto generation = channel.generation();

        const char byte = 'x';
        check(::write(write_fd.get(), &byte, 1) == 1, "pipe write should arm read event");
        (void)loop.loop();

        check(handler.read_count == 1, "real fd read event should dispatch once");
        check(!loop.accepts_poll_event_for_test({read_fd.get(), yuan::net::Channel::READ_EVENT, generation}),
              "real fd close should invalidate registered event token");
        channel.disable_all();
        loop.update_channel(&channel);
        check(handler.read_count == 1, "stale disabled update after close should not redispatch");
#endif
    }

    void test_real_fd_cross_thread_read_wakes_poller()
    {
        std::cout << "  [EventToken] real fd cross-thread read wakes poller\n";
#ifdef _WIN32
        check(true, "real fd wake test skipped on Windows");
#else
        int fds[2] = {-1, -1};
        if (!make_pipe(fds)) {
            check(false, "pipe should be available for real fd wake test");
            return;
        }
        FdGuard read_fd(fds[0]);
        FdGuard write_fd(fds[1]);

        yuan::net::PollPoller poller;
        check(poller.init(), "poll poller should initialize for wake test");
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        yuan::net::Channel channel(read_fd.get());
        PipeReadHandler handler(read_fd.get(), &channel);
        handler.quit_on_read = true;
        handler.set_event_handler(&loop);
        channel.set_handler(&handler);
        channel.enable_read();
        loop.update_channel(&channel);

        std::thread loop_thread([&loop]() {
            (void)loop.loop();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const char byte = 'y';
        check(::write(write_fd.get(), &byte, 1) == 1, "cross-thread pipe write should succeed");
        loop_thread.join();

        check(handler.read_count == 1, "real poller should wake and dispatch pipe read");
        loop.close_channel(&channel);
#endif
    }

    void test_real_fd_cross_thread_update_channel_wakes_poller()
    {
        std::cout << "  [EventToken] queued channel registration on loop thread\n";

        yuan::net::PollPoller poller;
        check(poller.init(), "poll poller should initialize for queued registration test");
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        UdpSelfSocket sock;
        check(sock.init(), "udp self socket should initialize for queued registration test");

        yuan::net::Channel channel(sock.fd());
        UdpReadHandler handler(&sock);
        handler.quit_on_read = true;
        handler.set_event_handler(&loop);
        channel.set_handler(&handler);
        channel.enable_read();

        sock.notify();

        std::thread loop_thread([&loop]() {
            (void)loop.loop();
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto started = std::chrono::steady_clock::now();
        loop.queue_in_loop([&loop, &channel]() {
            loop.update_channel(&channel);
        });
        loop_thread.join();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started).count();

        check(handler.read_count == 1, "queued registration should deliver new fd event");
        check(elapsed_ms < 1000, "loop should observe queued registration promptly");
    }

    void test_real_fd_cross_thread_close_channel_while_polling()
    {
        std::cout << "  [EventToken] queued close keeps loop quiet after deregistration\n";

        yuan::net::PollPoller poller;
        check(poller.init(), "poll poller should initialize for queued close test");
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        UdpSelfSocket sock;
        check(sock.init(), "udp self socket should initialize for queued close test");

        yuan::net::Channel channel(sock.fd());
        UdpReadHandler handler(&sock);
        handler.set_event_handler(&loop);
        channel.set_handler(&handler);
        channel.enable_read();
        loop.update_channel(&channel);

        std::thread loop_thread([&loop]() {
            (void)loop.loop();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        std::promise<void> closed;
        auto closed_future = closed.get_future();
        loop.queue_in_loop([&loop, &channel, &closed]() {
            loop.close_channel(&channel);
            closed.set_value();
        });
        closed_future.wait();

        sock.notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check(handler.read_count == 0, "queued closed channel must not receive events");

        loop.quit();
        loop_thread.join();
        check(handler.read_count == 0, "loop should stay quiet for closed channel until quit");
    }

    void test_external_fd_registration_raii_unregisters_without_closing_fd()
    {
        std::cout << "  [EventToken] external fd registration raii unregisters only\n";
#ifdef _WIN32
        check(true, "external fd registration test skipped on Windows");
#else
        int fds[2] = {-1, -1};
        if (!make_pipe(fds)) {
            check(false, "pipe should be available for external fd registration test");
            return;
        }
        FdGuard read_fd(fds[0]);
        FdGuard write_fd(fds[1]);

        NoopPoller poller;
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        const int fd = read_fd.get();
        auto handler = std::make_shared<RegistrationPipeHandler>(fd);
        auto registration = loop.register_external_fd(fd, handler, yuan::net::Channel::READ_EVENT);
        check(registration != nullptr && registration->active(), "external fd registration should be active");
        const auto generation = registration->generation();
        check(loop.accepts_poll_event_for_test({fd, yuan::net::Channel::READ_EVENT, generation}),
              "external fd registration should register channel generation");

        registration.reset();
        check(!loop.accepts_poll_event_for_test({fd, yuan::net::Channel::READ_EVENT, generation}),
              "external fd registration destructor should unregister channel");

        const char byte = 'z';
        check(::write(write_fd.get(), &byte, 1) == 1, "registration close should not close caller-owned fd");
#endif
    }

    void test_external_fd_registration_close_inside_callback()
    {
        std::cout << "  [EventToken] external fd registration callback close\n";
#ifdef _WIN32
        check(true, "external fd callback close test skipped on Windows");
#else
        int fds[2] = {-1, -1};
        if (!make_pipe(fds)) {
            check(false, "pipe should be available for external fd callback close test");
            return;
        }
        FdGuard read_fd(fds[0]);
        FdGuard write_fd(fds[1]);

        yuan::net::PollPoller poller;
        check(poller.init(), "poll poller should initialize for external fd callback close test");
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::EventLoop loop(&poller, &timer_manager);

        auto handler = std::make_shared<RegistrationPipeHandler>(read_fd.get());
        handler->quit_on_read = true;
        std::unique_ptr<yuan::net::EventLoop::ExternalFdRegistration> registration;
        handler->on_read = [&registration]() {
            if (registration) {
                registration->close();
            }
        };
        registration = loop.register_external_fd(read_fd.get(), handler, yuan::net::Channel::READ_EVENT);
        check(registration != nullptr && registration->active(), "external fd callback registration should be active");
        const auto generation = registration->generation();

        const char byte = 'q';
        check(::write(write_fd.get(), &byte, 1) == 1, "pipe write should trigger external fd callback");
        (void)loop.loop();

        check(handler->read_count == 1, "external fd callback should dispatch once");
        check(registration && !registration->active(), "external fd callback close should mark registration inactive");
        check(!loop.accepts_poll_event_for_test({read_fd.get(), yuan::net::Channel::READ_EVENT, generation}),
              "external fd callback close should invalidate generation");
        registration.reset();
        check(handler->read_count == 1, "external fd registration destructor after callback close should be idempotent");
#endif
    }
}

int main()
{
    std::cout << "Running event token tests...\n\n";
    test_generation_validation();
    test_reused_fd_stale_channel_is_ignored();
    test_callback_close_invalidates_later_events();
    test_cross_thread_queue_wakes_idle_loop();
    test_quit_before_loop_is_preserved();
    test_cross_thread_sync_operation_and_close();
    test_real_fd_read_close_lifecycle();
    test_real_fd_cross_thread_read_wakes_poller();
    test_real_fd_cross_thread_update_channel_wakes_poller();
    test_real_fd_cross_thread_close_channel_while_polling();
    test_external_fd_registration_raii_unregisters_without_closing_fd();
    test_external_fd_registration_close_inside_callback();

    if (g_failed > 0) {
        std::cout << "FAILED: " << g_failed << " assertions failed\n";
        return 1;
    }

    std::cout << "All event token tests passed (" << g_passed << " assertions ok)\n";
    return 0;
}
