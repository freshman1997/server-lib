#include "app/app.h"
#include "net/event/event_loop.h"
#include "timer/wheel_timer_manager.h"

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
        }

        ~AppData()
        {
            if (timer_manager_) {
                delete timer_manager_;
                timer_manager_ = nullptr;
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