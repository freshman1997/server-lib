#ifndef __SESSION_H__
#define __SESSION_H__
#include <string>
#include <unordered_map>

#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "timer/timer_task.h"

namespace net::http 
{
    enum class SessionItemType : char
    {
        invalid = -1,
        ival,
        dval,
        sval,
    };

    struct SessionItem
    {
        SessionItemType type = SessionItemType::invalid;
        union 
        {
            int     ival;
            double  dval;
        } 
        number;
        std::string sval;
    };

    class HttpSessionContext;

    class HttpSession : public timer::TimerTask
    {
    public:
        HttpSession(uint64_t id, HttpSessionContext *context, timer::TimerManager *timer_manager);
        ~HttpSession();
        
        void add_session_value(const std::string &key, int ival);
        void add_session_value(const std::string &key, double dval);
        void add_session_value(const std::string &key, const std::string &sval);

        uint64_t get_session_id() const 
        {
            return session_id_;
        }

        HttpSessionContext * get_context()
        {
            return context_;
        }

        timer::TimerManager * get_timer_manager()
        {
            return timer_manager_;
        }

    public:
        void on_timer(timer::Timer *timer);

        virtual void on_finished(timer::Timer *timer)
        {

        }

    public:
        void reset_timer();

    private:
        uint64_t session_id_;
        std::unordered_map<std::string, SessionItem> session_items_;
        HttpSessionContext *context_;
        timer::TimerManager *timer_manager_;
        timer::Timer *conn_timer_;
    };
}

#endif
