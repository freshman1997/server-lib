#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "base/spinlock.h"
#include "logger.h"
#include "net/channel/channel.h"
#include "event/event_loop.h"
#include "net/handler/select_handler.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/stream_listener.h"
#include "platform/native_platform.h"

#include <ranges>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace yuan::net 
{
    namespace
    {
        constexpr uint32_t kInitialIdlePollTimeoutMs = 1;
        constexpr uint32_t kMaxIdlePollTimeoutMs = 50;
        constexpr uint32_t kActiveTimerPollTimeoutCapMs = kMaxIdlePollTimeoutMs;
        constexpr uint32_t kEventDispatchBudgetMs = 8;
        // Saturation point of the idle backoff shift; idle_streak_ never grows
        // past this because the timeout is already capped by then.
        constexpr uint32_t kIdleBackoffShiftSaturation = 6;
    }

    class EventLoop::HelperData
    {
    public:
        enum class State : uint8_t
        {
            created,
            running,
            stopping,
            stopped,
        };

        HelperData() = default;
        HelperData(const HelperData &) = delete;
        HelperData & operator=(const HelperData &) = delete;

    public:
        std::atomic_bool quit_{false};
        std::atomic_bool resume_coroutine_requested_{false};
        std::atomic_bool loop_running_{false};
        std::atomic<State> state_{State::created};
        mutable std::mutex lifecycle_mutex_;
        mutable std::condition_variable lifecycle_cv_;
        // Serializes setup/teardown operations with the created -> running
        // transition. It is recursive because setup callbacks can call back
        // into run_in_loop_sync while configuring an object.
        mutable std::recursive_mutex setup_mutex_;
        std::atomic_size_t channel_count_{0};
        std::atomic_bool has_pending_callbacks_{false};
        std::atomic_bool has_pending_coroutines_{false};
        Poller *poller_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        // Guards pending queues and channel maps. Runtime channel mutations
        // are marshalled synchronously to the loop thread; setup/teardown
        // mutations may run directly when the loop is not running.
        yuan::base::Spinlock spinlock_;
        std::unordered_map<int, Channel *> channels_;
        std::unordered_set<int> tombstoned_fds_;
        std::queue<std::function<void()>> pending_callbacks_;
        std::queue<std::coroutine_handle<>> pending_coroutines_;
        std::vector<PollEvent> deferred_events_;
        std::unordered_map<int, std::shared_ptr<Connection>> connections_;
        uint64_t next_generation_ = 1;
        uint32_t idle_streak_ = 0;
        std::atomic<std::thread::id> loop_thread_id_;
#ifdef _WIN32
        int wakeup_fd_ = -1;
        sockaddr_in wakeup_addr_{};
        bool wakeup_wsa_started_ = false;
        Channel wakeup_channel_;
#else
        int wakeup_read_fd_ = -1;
        int wakeup_write_fd_ = -1;
        Channel wakeup_channel_;
#endif

        uint64_t next_generation() noexcept
        {
            const uint64_t generation = next_generation_++;
            if (next_generation_ == 0) {
                next_generation_ = 1;
            }
            return generation == 0 ? 1 : generation;
        }

        void register_channel_locked(Channel *channel)
        {
            const int fd = channel->get_fd();
            auto it = channels_.find(fd);
            if (it == channels_.end() || it->second != channel) {
                channel->set_generation(next_generation());
            }
            if (it == channels_.end()) {
                channel_count_.fetch_add(1, std::memory_order_release);
            }
            poller_->update_channel(channel);
            channels_[fd] = channel;
            tombstoned_fds_.erase(fd);
        }

        bool remove_registered_channel_locked(Channel *channel, const bool erase_connection)
        {
            const int fd = channel->get_fd();
            auto it = channels_.find(fd);
            if (it == channels_.end() || it->second != channel) {
                return false;
            }

            poller_->remove_channel(channel);
            channels_.erase(it);
            channel_count_.fetch_sub(1, std::memory_order_release);
            tombstoned_fds_.insert(fd);
            channel->bump_generation();
            if (erase_connection) {
                connections_.erase(fd);
            }
            return true;
        }

        bool init_wakeup_fd()
        {
#ifdef _WIN32
            if (!wakeup_wsa_started_) {
                WSADATA wsa{};
                if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                    return false;
                }
                wakeup_wsa_started_ = true;
            }

            const SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (fd == INVALID_SOCKET) {
                return false;
            }

            u_long non_blocking = 1;
            if (::ioctlsocket(fd, FIONBIO, &non_blocking) != 0) {
                ::closesocket(fd);
                return false;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
                ::closesocket(fd);
                return false;
            }

            int addr_len = static_cast<int>(sizeof(wakeup_addr_));
            if (::getsockname(fd, reinterpret_cast<sockaddr *>(&wakeup_addr_), &addr_len) != 0) {
                ::closesocket(fd);
                return false;
            }

            wakeup_fd_ = static_cast<int>(fd);
            wakeup_channel_.set_fd(wakeup_fd_);
            wakeup_channel_.enable_read();
            return true;
#else
            int fds[2] = { -1, -1 };
            if (::pipe(fds) != 0) {
                return false;
            }
            wakeup_read_fd_ = fds[0];
            wakeup_write_fd_ = fds[1];
            ::fcntl(wakeup_read_fd_, F_SETFL, ::fcntl(wakeup_read_fd_, F_GETFL, 0) | O_NONBLOCK);
            ::fcntl(wakeup_write_fd_, F_SETFL, ::fcntl(wakeup_write_fd_, F_GETFL, 0) | O_NONBLOCK);
            wakeup_channel_.set_fd(wakeup_read_fd_);
            wakeup_channel_.enable_read();
            return true;
#endif
        }

        void close_wakeup_fd()
        {
#ifdef _WIN32
            if (wakeup_fd_ != -1) {
                ::closesocket(static_cast<SOCKET>(wakeup_fd_));
                wakeup_fd_ = -1;
            }
            if (wakeup_wsa_started_) {
                ::WSACleanup();
                wakeup_wsa_started_ = false;
            }
#else
            if (wakeup_read_fd_ != -1) {
                ::close(wakeup_read_fd_);
                wakeup_read_fd_ = -1;
            }
            if (wakeup_write_fd_ != -1) {
                ::close(wakeup_write_fd_);
                wakeup_write_fd_ = -1;
            }
#endif
        }

        bool is_wakeup_fd(int fd) const noexcept
        {
#ifdef _WIN32
            return wakeup_fd_ != -1 && fd == wakeup_fd_;
#else
            return wakeup_read_fd_ != -1 && fd == wakeup_read_fd_;
#endif
        }

        void drain_wakeup_fd() noexcept
        {
#ifdef _WIN32
            if (wakeup_fd_ == -1) {
                return;
            }
            char buf[128];
            for (;;) {
                const auto n = ::recvfrom(static_cast<SOCKET>(wakeup_fd_), buf, sizeof(buf), 0, nullptr, nullptr);
                if (n > 0) {
                    continue;
                }
                if (n < 0 && platform::GetLastNativeError() == WSAEINTR) {
                    continue;
                }
                break;
            }
#else
            if (wakeup_read_fd_ == -1) {
                return;
            }
            char buf[128];
            for (;;) {
                const auto n = ::read(wakeup_read_fd_, buf, sizeof(buf));
                if (n > 0) {
                    continue;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
#endif
        }

        void notify_wakeup_fd() noexcept
        {
#ifdef _WIN32
            if (wakeup_fd_ == -1) {
                return;
            }
            const char byte = 1;
            for (;;) {
                const auto n = ::sendto(static_cast<SOCKET>(wakeup_fd_), &byte, sizeof(byte), 0,
                                        reinterpret_cast<sockaddr *>(&wakeup_addr_), sizeof(wakeup_addr_));
                if (n == 1) {
                    return;
                }
                const int err = platform::GetLastNativeError();
                if (err == WSAEINTR) {
                    continue;
                }
                return;
            }
#else
            if (wakeup_write_fd_ == -1) {
                return;
            }
            const char byte = 1;
            for (;;) {
                const auto n = ::write(wakeup_write_fd_, &byte, sizeof(byte));
                if (n == 1 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                    return;
                }
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                return;
            }
#endif
        }
    };

    EventLoop::EventLoop(Poller *poller, timer::TimerManager *timer_manager)
    {
        data_ = std::make_unique<EventLoop::HelperData>();
        data_->poller_ = poller;
        data_->timer_manager_ = timer_manager;
        data_->quit_ = false;
        if (data_->poller_ && data_->init_wakeup_fd()) {
            std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
            data_->register_channel_locked(&data_->wakeup_channel_);
        }
    }

    EventLoop::~EventLoop()
    {
        if (data_->loop_running_.load(std::memory_order_acquire)) {
            LOG_ERROR("EventLoop destroyed while its loop thread is still running");
            std::terminate();
        }
        std::unordered_map<int, std::shared_ptr<Connection>> connections;
        {
            std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
            connections.swap(data_->connections_);
            if (data_->is_wakeup_fd(data_->wakeup_channel_.get_fd())) {
                data_->remove_registered_channel_locked(&data_->wakeup_channel_, false);
            }
            data_->channels_.clear();
            data_->channel_count_.store(0, std::memory_order_release);
            data_->tombstoned_fds_.clear();
            data_->pending_callbacks_ = {};
            data_->pending_coroutines_ = {};
            data_->deferred_events_.clear();
            data_->has_pending_callbacks_.store(false, std::memory_order_release);
            data_->has_pending_coroutines_.store(false, std::memory_order_release);
        }

        for (auto &val : connections | std::views::values) {
            if (val) {
                val->detach_owner_event_handler();
            }
        }
        data_->close_wakeup_fd();
    }

    EventLoopExitReason EventLoop::loop()
    {
        assert(data_->poller_);

        std::unique_lock<std::recursive_mutex> setup_lock(data_->setup_mutex_);
        {
            std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
            const auto state = data_->state_.load(std::memory_order_relaxed);
            if (state == HelperData::State::running || state == HelperData::State::stopping) {
                LOG_ERROR("EventLoop::loop called concurrently");
                return EventLoopExitReason::quit_requested;
            }
            // A stopped loop may be entered again after a coroutine-resume
            // exit. A quit requested before the first start must not be
            // erased, while flags from a completed run are reset here.
            if (state == HelperData::State::stopped) {
                data_->resume_coroutine_requested_.store(false, std::memory_order_relaxed);
            }
            data_->state_.store(HelperData::State::running, std::memory_order_release);
            data_->loop_running_.store(true, std::memory_order_release);
        }
        setup_lock.unlock();
        data_->resume_coroutine_requested_.store(false, std::memory_order_relaxed);
        data_->loop_thread_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);

        auto drain_callbacks = [this]() {
            if (!data_->has_pending_callbacks_.load(std::memory_order_acquire)) {
                return false;
            }

            std::queue<std::function<void()>> callbacks;
            {
                std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
                callbacks.swap(data_->pending_callbacks_);
                data_->has_pending_callbacks_.store(false, std::memory_order_release);
            }

            bool processed = false;
            while (!callbacks.empty()) {
                auto cb = std::move(callbacks.front());
                callbacks.pop();
                if (cb) {
                    processed = true;
                    try {
                        cb();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in pending callback: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in pending callback");
                    }
                }
            }

            return processed;
        };

        auto drain_coroutines = [this]() {
            if (!data_->has_pending_coroutines_.load(std::memory_order_acquire)) {
                return false;
            }

            std::queue<std::coroutine_handle<>> coroutines;
            {
                std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
                coroutines.swap(data_->pending_coroutines_);
                data_->has_pending_coroutines_.store(false, std::memory_order_release);
            }

            bool processed = false;
            while (!coroutines.empty()) {
                auto handle = coroutines.front();
                coroutines.pop();
                if (handle && !handle.done()) {
                    processed = true;
                    try {
                        handle.resume();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in pending coroutine: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in pending coroutine");
                    }
                }
            }

            return processed;
        };
        
        std::vector<PollEvent> events;
        events.reserve(4096);

        std::vector<PollEvent> active_events;
        active_events.reserve(256);

        auto has_channels = [this]() {
            return data_->channel_count_.load(std::memory_order_acquire) > 0;
        };

        auto idle_timeout = [this]() {
            const auto shift = data_->idle_streak_ < kIdleBackoffShiftSaturation
                ? data_->idle_streak_
                : kIdleBackoffShiftSaturation;
            const auto timeout = kInitialIdlePollTimeoutMs << shift;
            return timeout > kMaxIdlePollTimeoutMs ? kMaxIdlePollTimeoutMs : timeout;
        };

        auto poll_timeout = [this, &idle_timeout](const bool processed_work) {
            if (processed_work) {
                return 0U;
            }

            if (!data_->timer_manager_) {
                return idle_timeout();
            }

            return data_->timer_manager_->poll_timeout(idle_timeout(), kActiveTimerPollTimeoutCapMs);
        };

        while (!data_->quit_.load(std::memory_order_acquire) && !data_->resume_coroutine_requested_.load(std::memory_order_acquire)) {
            if (data_->timer_manager_) {
                data_->timer_manager_->run_due_timers();
            }

            bool processed_work = drain_callbacks();
            processed_work = drain_coroutines() || processed_work;

            events.clear();
            active_events.clear();
            if (!data_->deferred_events_.empty()) {
                active_events.swap(data_->deferred_events_);
            }

            if (active_events.empty()) {
                const bool has_registered_channels = has_channels();
                const uint32_t timeout_ms = poll_timeout(processed_work);
                if (!has_registered_channels) {
                    // Degraded mode (wakeup fd unavailable): bounded sleep +
                    // atomic flag re-check each iteration keeps the loop
                    // responsive without a condition variable.
                    if (timeout_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
                    }
                    if (!processed_work && data_->idle_streak_ < kIdleBackoffShiftSaturation) {
                        ++data_->idle_streak_;
                    }
                    continue;
                }

                data_->poller_->poll(timeout_ms, events);
                if (!events.empty()) {
                    // channels_ is loop-thread-only while running; no lock needed.
                    for (const auto &event : events) {
                        if (data_->is_wakeup_fd(event.fd)) {
                            active_events.push_back(event);
                            continue;
                        }
                        auto it = data_->channels_.find(event.fd);
                        if (it == data_->channels_.end()) {
                            continue;
                        }

                        if (event.generation == 0 || it->second->generation() != event.generation) {
                            continue;
                        }

                        if (!it->second->has_events() ||
                            (it->second->get_events() & event.revents) == Channel::NONE_EVENT) {
                            continue;
                        }

                        active_events.push_back(event);
                    }
                }
            }

            if (!active_events.empty()) {
                const auto event_dispatch_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kEventDispatchBudgetMs);
                std::size_t dispatched_events = 0;
                for (std::size_t event_index = 0; event_index < active_events.size(); ++event_index) {
                    if (dispatched_events > 0 && std::chrono::steady_clock::now() >= event_dispatch_deadline) {
                        data_->deferred_events_.insert(data_->deferred_events_.end(),
                                                      active_events.begin() + static_cast<std::ptrdiff_t>(event_index),
                                                      active_events.end());
                        processed_work = true;
                        break;
                    }

                    const auto &active_event = active_events[event_index];
                    if (data_->is_wakeup_fd(active_event.fd)) {
                        data_->drain_wakeup_fd();
                        processed_work = true;
                        ++dispatched_events;
                        continue;
                    }
                    Channel *channel = nullptr;
                    std::shared_ptr<Connection> pinned_connection;
                    {
                        // Loop-thread-only state; no lock needed.
                        auto it = data_->channels_.find(active_event.fd);
                        if (it == data_->channels_.end()) {
                            continue;
                        }

                        channel = it->second;

                        if (!channel || active_event.generation == 0 ||
                            channel->generation() != active_event.generation ||
                            !channel->has_events() ||
                            (channel->get_events() & active_event.revents) == Channel::NONE_EVENT) {
                            continue;
                        }

                        auto connection_it = data_->connections_.find(active_event.fd);
                        if (connection_it != data_->connections_.end()) {
                            pinned_connection = connection_it->second;
                        }
                    }

                    processed_work = true;
                    ++dispatched_events;
                    try {
                        channel->set_revent(active_event.revents);
                        channel->on_event();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in event loop (fd={}): {}", channel->get_fd(), e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in event loop (fd={})", channel->get_fd());
                    }
                }
            }

            processed_work = drain_callbacks() || processed_work;
            processed_work = drain_coroutines() || processed_work;

            if (data_->timer_manager_) {
                data_->timer_manager_->run_due_timers();
            }

            if (processed_work) {
                data_->idle_streak_ = 0;
            } else if (data_->idle_streak_ < kIdleBackoffShiftSaturation) {
                ++data_->idle_streak_;
            }
        }

        for (;;) {
            const bool callbacks_processed = drain_callbacks();
            const bool coroutines_processed = drain_coroutines();
            if (!callbacks_processed && !coroutines_processed) {
                std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
                std::lock_guard<yuan::base::Spinlock> queue_lock(data_->spinlock_);
                if (!data_->pending_callbacks_.empty() || !data_->pending_coroutines_.empty()) {
                    continue;
                }
                data_->loop_running_.store(false, std::memory_order_release);
                data_->state_.store(HelperData::State::stopped, std::memory_order_release);
                data_->loop_thread_id_.store(std::thread::id{}, std::memory_order_release);
                break;
            }
        }

        data_->lifecycle_cv_.notify_all();

        return data_->resume_coroutine_requested_.load(std::memory_order_acquire)
            ? EventLoopExitReason::coroutine_resume_requested
            : EventLoopExitReason::quit_requested;
    }

    void EventLoop::on_new_connection(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
            return;
        }

        if (!is_in_loop_thread() && data_->loop_running_.load(std::memory_order_acquire)) {
            const auto keepalive = conn;
            run_in_loop_sync([this, keepalive]() {
                on_new_connection(keepalive);
            });
            return;
        }

        auto stream = std::dynamic_pointer_cast<StreamTransport>(conn);
        Channel *channel = stream ? stream->stream_channel() : nullptr;

        const InetAddress &addr = conn->get_remote_address();
        if (channel) {
            LOG_INFO("new connection, ip: {}, port: {}, fd: {}", addr.get_ip(), addr.get_port(), channel->get_fd());
        } else {
            LOG_INFO("new connection, ip: {}, port: {}", addr.get_ip(), addr.get_port());
        }

        if (channel) {
            std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
            data_->connections_[channel->get_fd()] = conn;
            data_->register_channel_locked(channel);
        }
    }

    void EventLoop::quit()
    {
        data_->quit_.store(true, std::memory_order_release);
        wakeup();
    }

    void EventLoop::close_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        if (!is_in_loop_thread() && data_->loop_running_.load(std::memory_order_acquire)) {
            run_in_loop_sync([this, channel]() { close_channel(channel); });
            return;
        }
        {
            std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
            const int fd = channel->get_fd();
            auto it = data_->channels_.find(fd);
            if (it != data_->channels_.end()) {
                if (it->second != channel) {
                    channel->bump_generation();
                    LOG_DEBUG("ignore stale close_channel for reused fd: {}", fd);
                    return;
                }
                LOG_INFO("channel closed, fd: {}", fd);
                data_->poller_->remove_channel(channel);
                data_->channels_.erase(it);
                data_->channel_count_.fetch_sub(1, std::memory_order_release);
                data_->tombstoned_fds_.insert(fd);
                channel->bump_generation();
                data_->connections_.erase(fd);
            } else if (data_->tombstoned_fds_.find(fd) != data_->tombstoned_fds_.end()) {
                channel->bump_generation();
            } else {
                LOG_WARN("channel not found, fd: {}", fd);
            }
        }
    }

    void EventLoop::update_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        if (!is_in_loop_thread() && data_->loop_running_.load(std::memory_order_acquire)) {
            run_in_loop_sync([this, channel]() { update_channel(channel); });
            return;
        }
        {
            std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
            const int fd = channel->get_fd();
            if (!channel->has_events()) {
                auto it = data_->channels_.find(fd);
                if (it != data_->channels_.end() && it->second == channel) {
                    data_->poller_->remove_channel(channel);
                    data_->channels_.erase(it);
                    data_->channel_count_.fetch_sub(1, std::memory_order_release);
                    data_->tombstoned_fds_.insert(fd);
                    channel->bump_generation();
                } else if (data_->tombstoned_fds_.find(fd) != data_->tombstoned_fds_.end()) {
                    channel->bump_generation();
                }
            } else {
                data_->register_channel_locked(channel);
            }
        }
    }

    void EventLoop::wakeup()
    {
        data_->notify_wakeup_fd();
    }

    void EventLoop::request_coroutine_resume()
    {
        data_->resume_coroutine_requested_.store(true, std::memory_order_release);
        wakeup();
    }

    void EventLoop::queue_in_loop(std::function<void()> cb)
    {
        if (!cb) {
            return;
        }

        {
            std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
            const auto state = data_->state_.load(std::memory_order_relaxed);
            if (state == HelperData::State::stopped && data_->quit_.load(std::memory_order_acquire)) {
                // The stopped state is published only after the final drain.
                return;
            }

            std::lock_guard<yuan::base::Spinlock> queue_lock(data_->spinlock_);
            data_->pending_callbacks_.push(std::move(cb));
            data_->has_pending_callbacks_.store(true, std::memory_order_release);
        }

        if (!is_in_loop_thread()) {
            wakeup();
        }
    }

    void EventLoop::post_coroutine(std::coroutine_handle<> handle) noexcept
    {
        if (!handle) {
            return;
        }

        {
            std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
            const auto state = data_->state_.load(std::memory_order_relaxed);
            if (state == HelperData::State::stopped && data_->quit_.load(std::memory_order_acquire)) {
                return;
            }

            std::lock_guard<yuan::base::Spinlock> queue_lock(data_->spinlock_);
            data_->pending_coroutines_.push(handle);
            data_->has_pending_coroutines_.store(true, std::memory_order_release);
        }

        if (!is_in_loop_thread()) {
            wakeup();
        }
    }

    bool EventLoop::run_in_loop_sync(std::function<void()> operation)
    {
        if (!operation) {
            return false;
        }
        if (is_in_loop_thread()) {
            operation();
            return true;
        }

        // Keep setup and the startup transition mutually exclusive. Without
        // this gate a caller can observe `created`, release the lifecycle
        // lock, and mutate poller-owned state concurrently with loop().
        std::unique_lock<std::recursive_mutex> setup_lock(data_->setup_mutex_);
        bool run_without_loop = false;
        {
            std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
            const auto state = data_->state_.load(std::memory_order_relaxed);
            if (state == HelperData::State::created || state == HelperData::State::stopped) {
                // No loop thread owns poller state in either setup state. A
                // stopped loop remains reusable unless quit is explicitly
                // requested; teardown operations are still safe after quit.
                run_without_loop = true;
            }
        }

        if (run_without_loop) {
            operation();
            return true;
        }

        if (is_in_loop_thread()) {
            operation();
            return true;
        }

        auto completion = std::make_shared<std::promise<void>>();
        auto future = completion->get_future();
        {
            std::lock_guard<std::mutex> lifecycle_lock(data_->lifecycle_mutex_);
            const auto state = data_->state_.load(std::memory_order_relaxed);
            if (state != HelperData::State::running && state != HelperData::State::stopping) {
                return false;
            }

            std::lock_guard<yuan::base::Spinlock> queue_lock(data_->spinlock_);
            data_->pending_callbacks_.push([operation = std::move(operation), completion]() mutable {
                try {
                    operation();
                } catch (...) {
                    completion->set_exception(std::current_exception());
                    return;
                }
                completion->set_value();
            });
            data_->has_pending_callbacks_.store(true, std::memory_order_release);
        }
        wakeup();
        try {
            future.get();
        } catch (...) {
            return false;
        }
        return true;
    }

    bool EventLoop::is_in_loop_thread() const noexcept
    {
        return data_->loop_thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    bool EventLoop::is_running() const noexcept
    {
        return data_->loop_running_.load(std::memory_order_acquire);
    }

    bool EventLoop::wait_until_stopped(uint32_t timeout_ms) const
    {
        if (is_in_loop_thread()) {
            return false;
        }
        std::unique_lock<std::mutex> lock(data_->lifecycle_mutex_);
        auto stopped = [this]() {
            const auto state = data_->state_.load(std::memory_order_acquire);
            return state == HelperData::State::created || state == HelperData::State::stopped;
        };
        if (timeout_ms == 0) {
            data_->lifecycle_cv_.wait(lock, stopped);
            return true;
        }
        return data_->lifecycle_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), stopped);
    }

    bool EventLoop::accepts_poll_event_for_test(const PollEvent &event) const
    {
        if (data_->loop_running_.load(std::memory_order_acquire) && !is_in_loop_thread()) {
            LOG_ERROR("accepts_poll_event_for_test called from a foreign thread while the loop is running");
            return false;
        }
        std::lock_guard<yuan::base::Spinlock> lock(data_->spinlock_);
        auto it = data_->channels_.find(event.fd);
        return it != data_->channels_.end() && event.generation != 0 &&
               it->second && it->second->has_events() &&
               it->second->generation() == event.generation &&
               (it->second->get_events() & event.revents) != Channel::NONE_EVENT;
    }

    std::unique_ptr<EventLoop::ExternalFdRegistration> EventLoop::register_external_fd(
        int fd,
        std::shared_ptr<SelectHandler> handler,
        int events)
    {
        if (fd < 0 || !handler || events == Channel::NONE_EVENT) {
            return nullptr;
        }
        return std::make_unique<ExternalFdRegistration>(this, fd, std::move(handler), events);
    }

    struct ExternalFdRegistrationState
    {
        enum class Status : uint8_t
        {
            pending,
            registered,
            closing,
            closed,
        };

        EventLoop *loop = nullptr;
        std::shared_ptr<SelectHandler> handler;
        std::unique_ptr<Channel> channel;
        Status status = Status::pending;

        void register_on_loop()
        {
            if (!loop || !channel || status != Status::pending) {
                return;
            }

            handler->set_event_handler(loop);
            channel->set_handler(std::weak_ptr<SelectHandler>(handler));
            loop->update_channel(channel.get());
            status = Status::registered;
        }

        void close_on_loop()
        {
            if (!channel || status == Status::closed) {
                return;
            }

            const auto previous = status;
            status = Status::closing;
            if (loop && previous == Status::registered) {
                loop->close_channel(channel.get());
            }
            channel->disable_all();
            channel->clear_handler();
            handler.reset();
            status = Status::closed;
        }
    };

    struct EventLoop::ExternalFdRegistration::Impl
    {
        EventLoop *loop = nullptr;
        std::shared_ptr<ExternalFdRegistrationState> state;
        std::atomic<bool> active{false};
    };

    EventLoop::ExternalFdRegistration::ExternalFdRegistration(
        EventLoop *loop,
        int fd,
        std::shared_ptr<SelectHandler> handler,
        int events)
        : impl_(std::make_unique<Impl>())
    {
        impl_->loop = loop;
        impl_->state = std::make_shared<ExternalFdRegistrationState>();
        impl_->active.store(loop != nullptr && handler != nullptr && events != Channel::NONE_EVENT,
                            std::memory_order_release);
        if (!impl_->active.load(std::memory_order_acquire)) {
            impl_->state.reset();
            return;
        }

        impl_->state->loop = loop;
        impl_->state->handler = std::move(handler);
        impl_->state->channel = std::make_unique<Channel>(fd);
        if (events & Channel::READ_EVENT) {
            impl_->state->channel->enable_read();
        }

        if (events & Channel::WRITE_EVENT) {
            impl_->state->channel->enable_write();
        }

        if (loop->is_in_loop_thread() || !loop->is_running()) {
            impl_->state->register_on_loop();
        } else {
            auto state = impl_->state;
            if (!loop->run_in_loop_sync([state]() {
                state->register_on_loop();
            })) {
                impl_->active.store(false, std::memory_order_release);
                impl_->state->close_on_loop();
                impl_->state.reset();
            }
        }
    }

    EventLoop::ExternalFdRegistration::~ExternalFdRegistration()
    {
        close();
    }

    void EventLoop::ExternalFdRegistration::close()
    {
        if (!impl_ || !impl_->active.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        if (!impl_->state) {
            return;
        }

        if (!impl_->loop || !impl_->loop->is_running() || impl_->loop->is_in_loop_thread()) {
            impl_->state->close_on_loop();
            impl_->state.reset();
        } else {
            auto state = impl_->state;
            if (!impl_->loop->run_in_loop_sync([state]() {
                state->close_on_loop();
            })) {
                impl_->loop->wait_until_stopped();
                impl_->state->close_on_loop();
                impl_->state.reset();
                return;
            }
            impl_->state.reset();
        }
    }

    bool EventLoop::ExternalFdRegistration::active() const noexcept
    {
        return impl_ && impl_->active.load(std::memory_order_acquire);
    }

    Channel *EventLoop::ExternalFdRegistration::channel() noexcept
    {
        return impl_ && impl_->state ? impl_->state->channel.get() : nullptr;
    }

    uint64_t EventLoop::ExternalFdRegistration::generation() const noexcept
    {
        return impl_ && impl_->state && impl_->state->channel
            ? impl_->state->channel->generation()
            : 0;
    }
}
