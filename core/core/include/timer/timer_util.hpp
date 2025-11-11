#include "timer_task.h"
#include "timer.h"
#include "timer_manager.h"
#include <functional>

namespace yuan::timer 
{
    class TimerManager;
    class Timer;

    class TimerUtil
    {
    public:
        template<typename T>
        static Timer * build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, T *object, void (T::*func)(Timer *));

        template<typename T>
        static Timer * build_timeout_timer(TimerManager *manager, uint32_t timeout, T *object, void (T::*func)(Timer *));

        inline static Timer * build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, std::function<void (Timer *)> func, int period = 0);

        inline static Timer * build_timeout_timer(TimerManager *manager, uint32_t timeout, std::function<void (Timer *)> func);
    };

    template<class T>
    class DefaultTimerTask : public TimerTask
    {
    public:
        DefaultTimerTask(T *obj, void (T::*func)(Timer* timer)) : object_(obj), func_(func)
        {

        }

        virtual void on_timer(Timer *timer)
        {
            if (object_ && func_) {
                (object_->*func_)(timer);
            }
        }

        virtual void on_finished(Timer *timer)
        {
            delete this;
        }

        virtual bool need_free()
        {
            return true;
        }

    private:
        T *object_;
        void (T::*func_)(Timer* timer);
    };

    class NoObjectTimerTask : public TimerTask
    {
    public:
        NoObjectTimerTask(std::function<void (Timer *)> func) : func_(func)
        {

        }

        virtual void on_timer(Timer *timer)
        {
            func_(timer);
        }

        virtual void on_finished(Timer *timer)
        {
            delete this;
        }

        virtual bool need_free()
        {
            return true;
        }

    private:
        std::function<void (Timer *)> func_;
    };
    
    template<typename T>
    Timer * TimerUtil::build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, T *object, void (T::*func)(Timer *))
    {
        return manager->interval(timeout, interval, new DefaultTimerTask<T>(object, func), -1);
    }

    template<typename T>
    Timer * TimerUtil::build_timeout_timer(TimerManager *manager, uint32_t timeout, T *object, void (T::*func)(Timer *))
    {
        return manager->timeout(timeout, new DefaultTimerTask<T>(object, func));
    }

    Timer * TimerUtil::build_timeout_timer(TimerManager *manager, uint32_t timeout, std::function<void (Timer *)> func)
    {
        return manager->timeout(timeout, new NoObjectTimerTask(func));
    }

    Timer * TimerUtil::build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, std::function<void (Timer *)> func, int period)
    {
        return manager->interval(timeout, interval, new NoObjectTimerTask(func), period);
    }
}