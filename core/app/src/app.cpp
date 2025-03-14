#include "app.h"
#include "event/event_loop.h"
#include "timer/wheel_timer_manager.h"
#include "net/poller/epoll_poller.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <memory>

namespace yuan::app 
{
    class App::AppData
    {
    public:
        AppData()
        {
            timer_manager_ = new timer::WheelTimerManager;
            poller_ = new net::EpollPoller;
            loop_ = new net::EventLoop(poller_, timer_manager_);
        }

        ~AppData()
        {
            if (timer_manager_) {
                delete timer_manager_;
                timer_manager_ = nullptr;
            }

            if (poller_) {
                delete poller_;
                poller_ = nullptr;
            }

            if (loop_) {
                delete poller_;
                loop_ = nullptr;
            }
        }
        
    public:
        std::atomic_bool exit_ = false;
        net::Poller *poller_;
        net::EventLoop *loop_;
        timer::TimerManager *timer_manager_;
    };

    App::App() : data_(std::make_unique<App::AppData>())
    {}

    App::~App()
    {
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