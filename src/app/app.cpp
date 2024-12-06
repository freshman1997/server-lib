#include "app/app.h"
#include "net/acceptor/acceptor.h"
#include "net/event/event_loop.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <set>

namespace app 
{
    class App::AppData
    {
    public:
        std::atomic_bool exit_ = false;
        net::Poller *poller_;
        net::EventLoop *loop_;
        timer::TimerManager *timer_manager_;
        std::set<net::Acceptor *> acceptors_;
        std::set<net::Connector *> connectors_;
    };

    App::App() : data_(std::make_unique<App::AppData>())
    {}

    App::~App()
    {
    }

    bool App::add_acceptor(const net::InetAddress &addr)
    {
        return false;
    }
    
    bool App::add_connector(const net::InetAddress &addr)
    {
        return false;
    }

    void App::launch()
    {
        assert(data_->loop_);
        while (!data_->exit_.load()) {
            data_->loop_->loop();
            // TODO 做其他事情
        }
    }

    void App::exit()
    {
        data_->exit_.store(true);
    }
}