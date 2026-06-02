#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "logger.h"
#include "net/channel/channel.h"
#include "event/event_loop.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/stream_listener.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace yuan::net 
{
    namespace
    {
        constexpr uint32_t kIdlePollTimeoutMs = 50;
        constexpr uint32_t kActiveTimerPollTimeoutCapMs = 50;
    }

    class EventLoop::HelperData
    {
    public:
        HelperData() = default;
        HelperData(const HelperData &) = delete;
        HelperData & operator=(const HelperData &) = delete;

    public:
        std::atomic_bool quit_{false};
        std::atomic_bool resume_coroutine_requested_{false};
        std::atomic_bool is_waiting_{false};
        std::atomic_size_t channel_count_{0};
        Poller *poller_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        std::mutex m;
        std::condition_variable cond;
        std::unordered_map<int, Channel *> channels_;
        std::unordered_set<int> tombstoned_fds_;
        std::queue<std::function<void()>> pending_callbacks_;
        std::queue<std::coroutine_handle<>> pending_coroutines_;
        std::unordered_map<int, std::shared_ptr<Connection>> connections_;
        uint64_t next_generation_ = 1;

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
    };

    EventLoop::EventLoop(Poller *poller, timer::TimerManager *timer_manager)
    {
        data_ = std::make_unique<EventLoop::HelperData>();
        data_->poller_ = poller;
        data_->timer_manager_ = timer_manager;
        data_->quit_ = false;
        data_->is_waiting_ = false;
    }

    EventLoop::~EventLoop()
    {
        std::unordered_map<int, std::shared_ptr<Connection>> connections;
        {
            std::lock_guard<std::mutex> lock(data_->m);
            connections.swap(data_->connections_);
            data_->channels_.clear();
            data_->channel_count_.store(0, std::memory_order_release);
            data_->tombstoned_fds_.clear();
            data_->pending_callbacks_ = {};
            data_->pending_coroutines_ = {};
        }

        for (auto &entry : connections) {
            if (entry.second) {
                entry.second->abort();
            }
        }
    }

    EventLoopExitReason EventLoop::loop()
    {
        assert(data_->poller_);

        data_->quit_.store(false, std::memory_order_relaxed);
        data_->resume_coroutine_requested_.store(false, std::memory_order_relaxed);

        auto drain_callbacks = [this]() {
            std::queue<std::function<void()>> callbacks;
            {
                std::lock_guard<std::mutex> lock(data_->m);
                callbacks.swap(data_->pending_callbacks_);
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
            std::queue<std::coroutine_handle<>> coroutines;
            {
                std::lock_guard<std::mutex> lock(data_->m);
                coroutines.swap(data_->pending_coroutines_);
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
        auto has_channels = [this]() {
            return data_->channel_count_.load(std::memory_order_acquire) > 0;
        };

        auto poll_timeout = [this](const bool processed_work) {
            if (processed_work) {
                return 0U;
            }

            if (!data_->timer_manager_) {
                return kIdlePollTimeoutMs;
            }

            return data_->timer_manager_->poll_timeout(kIdlePollTimeoutMs, kActiveTimerPollTimeoutCapMs);
        };

        while (!data_->quit_.load(std::memory_order_acquire) && !data_->resume_coroutine_requested_.load(std::memory_order_acquire)) {
            if (data_->timer_manager_) {
                data_->timer_manager_->run_due_timers();
            }

            bool processed_work = drain_callbacks();
            processed_work = drain_coroutines() || processed_work;

            events.clear();
            const bool has_registered_channels = has_channels();
            const uint32_t timeout_ms = poll_timeout(processed_work);
            if (!has_registered_channels) {
                if (timeout_ms > 0) {
                    std::unique_lock<std::mutex> lock(data_->m);
                    data_->is_waiting_.store(true, std::memory_order_release);
                    data_->cond.wait_for(lock, std::chrono::milliseconds(timeout_ms));
                    data_->is_waiting_.store(false, std::memory_order_release);
                }
                continue;
            }

            data_->poller_->poll(timeout_ms, events);
            if (!events.empty()) {
                for (const auto &event : events) {
                    Channel *channel = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(data_->m);
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
                        channel = it->second;
                    }

                    if (channel) {
                        processed_work = true;
                        try {
                            channel->set_revent(event.revents);
                            channel->on_event();
                        } catch (const std::exception& e) {
                            LOG_ERROR("Exception in event loop (fd={}): {}", event.fd, e.what());
                        } catch (...) {
                            LOG_ERROR("Unknown exception in event loop (fd={})", event.fd);
                        }
                    }
                }
            }

            processed_work = drain_callbacks() || processed_work;
            processed_work = drain_coroutines() || processed_work;

            if (data_->timer_manager_) {
                data_->timer_manager_->run_due_timers();
            }
        }

        drain_callbacks();
        drain_coroutines();

        return data_->resume_coroutine_requested_.load(std::memory_order_acquire)
            ? EventLoopExitReason::coroutine_resume_requested
            : EventLoopExitReason::quit_requested;
    }

    void EventLoop::on_new_connection(const std::shared_ptr<Connection> &conn)
    {
        if (!conn) {
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
            std::lock_guard<std::mutex> lock(data_->m);
            data_->connections_[channel->get_fd()] = conn;
            data_->register_channel_locked(channel);
        }
    }

    void EventLoop::quit()
    {
        data_->quit_.store(true, std::memory_order_release);
        data_->cond.notify_all();
    }

    void EventLoop::close_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        std::lock_guard<std::mutex> lock(data_->m);
        const int fd = channel->get_fd();
        auto it = data_->channels_.find(fd);
        if (it != data_->channels_.end()) {
            if (it->second != channel) {
                channel->bump_generation();
                LOG_DEBUG("ignore stale close_channel for reused fd: {}", fd);
                return;
            }
            LOG_INFO("channel closed, fd: {}", fd);
            data_->remove_registered_channel_locked(channel, true);
        } else if (data_->tombstoned_fds_.find(fd) != data_->tombstoned_fds_.end()) {
            channel->bump_generation();
        } else {
            LOG_WARN("channel not found, fd: {}", fd);
        }
    }

    void EventLoop::update_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        std::lock_guard<std::mutex> lock(data_->m);
        const int fd = channel->get_fd();
        if (!channel->has_events()) {
            if (!data_->remove_registered_channel_locked(channel, false) &&
                data_->tombstoned_fds_.find(fd) != data_->tombstoned_fds_.end()) {
                channel->bump_generation();
            }
            return;
        }

        data_->register_channel_locked(channel);
    }

    void EventLoop::wakeup()
    {
        data_->cond.notify_all();
    }

    void EventLoop::request_coroutine_resume()
    {
        data_->resume_coroutine_requested_.store(true, std::memory_order_release);
        wakeup();
    }

    void EventLoop::queue_in_loop(std::function<void()> cb)
    {
        {
            std::lock_guard<std::mutex> lock(data_->m);
            data_->pending_callbacks_.push(std::move(cb));
        }
        wakeup();
    }

    void EventLoop::post_coroutine(std::coroutine_handle<> handle) noexcept
    {
        if (!handle) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(data_->m);
            data_->pending_coroutines_.push(handle);
        }
        wakeup();
    }

    bool EventLoop::accepts_poll_event_for_test(const PollEvent &event) const
    {
        std::lock_guard<std::mutex> lock(data_->m);
        auto it = data_->channels_.find(event.fd);
        return it != data_->channels_.end() && event.generation != 0 &&
               it->second && it->second->has_events() &&
               it->second->generation() == event.generation &&
               (it->second->get_events() & event.revents) != Channel::NONE_EVENT;
    }
}
