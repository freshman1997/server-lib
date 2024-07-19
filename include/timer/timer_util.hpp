#include "timer/timer_task.h"
#include "timer/timer_manager.h"

namespace timer 
{
    template<class T>
    class DefaultTimerTask : public TimerTask
    {
    public:
        DefaultTimerTask(T *obj, void (T::*func)(Timer* timer)) : object_(obj), func_(func)
        {

        }

        virtual void on_timer(Timer *timer)
        {
            (object_->*func_)(timer);
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
    
    template<typename T>
    Timer * TimerUtil::build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, T *object, void (T::*func)(Timer *))
    {
        return manager->interval(timeout, interval, new DefaultTimerTask(object, func), -1);
    }

    template<typename T>
    Timer * TimerUtil::build_timeout_timer(TimerManager *manager, uint32_t timeout, T *object, void (T::*func)(Timer *))
    {
        return manager->timeout(timeout, new DefaultTimerTask(object, func));
    }
}