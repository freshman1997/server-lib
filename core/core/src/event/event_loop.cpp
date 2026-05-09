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
#include "base/time.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace yuan::net 
{
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
        Poller *poller_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        std::mutex m;
        std::condition_variable cond;
        std::unordered_map<int, Channel *> channels_;
        std::unordered_set<int> tombstoned_fds_;
        std::queue<std::function<void()>> pending_callbacks_;
        std::queue<std::coroutine_handle<>> pending_coroutines_;
        std::unordered_map<int, std::shared_ptr<Connection>> connections_;
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

            while (!callbacks.empty()) {
                auto cb = std::move(callbacks.front());
                callbacks.pop();
                if (cb) {
                    try {
                        cb();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in pending callback: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in pending callback");
                    }
                }
            }
        };
        auto drain_coroutines = [this]() {
            std::queue<std::coroutine_handle<>> coroutines;
            {
                std::lock_guard<std::mutex> lock(data_->m);
                coroutines.swap(data_->pending_coroutines_);
            }

            while (!coroutines.empty()) {
                auto handle = coroutines.front();
                coroutines.pop();
                if (handle && !handle.done()) {
                    try {
                        handle.resume();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Exception in pending coroutine: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("Unknown exception in pending coroutine");
                    }
                }
            }
        };
        
        uint64_t from = base::time::get_tick_count();
        std::vector<PollEvent> events;
        events.reserve(4096);
        while (!data_->quit_.load(std::memory_order_acquire) &&
               !data_->resume_coroutine_requested_.load(std::memory_order_acquire)) {
            events.clear();
            const uint64_t to = data_->poller_->poll(2, events);
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
                        channel = it->second;
                    }

                    if (channel) {
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

            drain_callbacks();
            drain_coroutines();

            if (to - from < data_->timer_manager_->get_time_unit()) {
                std::unique_lock<std::mutex> lock(data_->m);
                data_->is_waiting_.store(true, std::memory_order_release);
                data_->cond.wait_for(lock, std::chrono::milliseconds(data_->timer_manager_->get_time_unit() - (to - from)));
                data_->is_waiting_.store(false, std::memory_order_release);
            }

            auto now = base::time::get_tick_count();
            if (now - from >= data_->timer_manager_->get_time_unit()) {
                from = now;
                data_->timer_manager_->tick();
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

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
            auto *channel = stream->stream_channel();
            if (channel) {
                std::lock_guard<std::mutex> lock(data_->m);
                data_->connections_[channel->get_fd()] = conn;
            }
        }

        const InetAddress &addr = conn->get_remote_address();
        Channel *channel = nullptr;

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
            channel = stream->stream_channel();
        }

        if (channel) {
            LOG_INFO("new connection, ip: {}, port: {}, fd: {}", addr.get_ip(), addr.get_port(), channel->get_fd());
        } else {
            LOG_INFO("new connection, ip: {}, port: {}", addr.get_ip(), addr.get_port());
        }

        if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
            channel = stream->stream_channel();
            if (!channel) {
                return;
            }
            std::lock_guard<std::mutex> lock(data_->m);
            data_->poller_->update_channel(channel);
            data_->channels_[channel->get_fd()] = channel;
            data_->tombstoned_fds_.erase(channel->get_fd());
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
        auto it = data_->channels_.find(channel->get_fd());
        if (it != data_->channels_.end()) {
            LOG_INFO("channel closed, fd: {}", channel->get_fd());
            data_->poller_->remove_channel(channel);
            data_->channels_.erase(it);
            data_->tombstoned_fds_.insert(channel->get_fd());
            channel->bump_generation();
            auto conn_it = data_->connections_.find(channel->get_fd());
            if (conn_it != data_->connections_.end()) {
                data_->connections_.erase(conn_it);
            }
        } else {
            LOG_WARN("channel not found, fd: {}", channel->get_fd());
        }
    }

    void EventLoop::update_channel(Channel *channel)
    {
        if (channel) {
            std::lock_guard<std::mutex> lock(data_->m);
            data_->channels_[channel->get_fd()] = channel;
            data_->tombstoned_fds_.erase(channel->get_fd());
            data_->poller_->update_channel(channel);
        }
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
               it->second && it->second->generation() == event.generation;
    }
}
