#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "net/channel/channel.h"
#include "event/event_loop.h"
#include "net/poller/poller.h"
#include "net/connection/connection.h"
#include "timer/timer_manager.h"
#include "net/socket/inet_address.h"
#include "net/acceptor/acceptor.h"
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
        bool quit_ = false;
        bool is_waiting_ = false;
        Poller *poller_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        std::mutex m;
        std::condition_variable cond;
        std::unordered_map<int, Channel *> channels_;
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
    }

    void EventLoop::loop()
    {
        assert(data_->poller_);

        uint64_t from = base::time::get_tick_count();
        std::vector<Channel *> channels;
        channels.reserve(4096);
        while (!data_->quit_) {
            channels.clear();
            uint64_t to = data_->poller_->poll(2, channels);
            if (!channels.empty()) {
                for (const auto &channel : channels) {
                    if (channel) {
                        channel->on_event();
                    }
                }
            }

            if (to - from >= data_->timer_manager_->get_time_unit()) {
                from = to;
                data_->timer_manager_->tick();
            }

            if (to - from < data_->timer_manager_->get_time_unit()) {
                {
                    std::unique_lock<std::mutex> lock(data_->m);
                    auto now = std::chrono::system_clock::now();
                    data_->is_waiting_ = true;
                    data_->cond.wait_until(lock, now + std::chrono::milliseconds(data_->timer_manager_->get_time_unit() - (to - from)));
                    data_->is_waiting_ = false;
                }
            }
        }
    }

    void EventLoop::on_new_connection(Connection *conn)
    {
        if (conn) {
            const InetAddress &addr = conn->get_remote_address();
            Channel * channel = conn->get_channel();

            if (channel) {
                std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << ", fd: " << channel->get_fd()<< std::endl;
            } else {
                std::cout << "new connection, ip: " << addr.get_ip() << ", port: " << addr.get_port() << std::endl;
            }
        
            if (conn->get_conn_type() == ConnectionType::TCP) {
                data_->poller_->update_channel(channel);
                data_->channels_[channel->get_fd()] = channel;
            }
        }
    }

    void EventLoop::quit()
    {
        data_->quit_ = true;
        data_->cond.notify_all();
    }

    void EventLoop::close_channel(Channel *channel)
    {
        if (!channel) {
            return;
        }

        auto it = data_->channels_.find(channel->get_fd());
        if (it != data_->channels_.end()) {
            data_->poller_->remove_channel(channel);
            data_->channels_.erase(it);
        }
    }

    void EventLoop::update_channel(Channel *channel)
    {
        if (channel) {
            data_->channels_[channel->get_fd()] = channel;
            data_->poller_->update_channel(channel);
        }
    }

    void EventLoop::wakeup()
    {
        data_->cond.notify_all();
    }
}