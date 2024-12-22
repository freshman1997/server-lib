#ifndef __SESSION_H__
#define __SESSION_H__
#include <string>
#include <unordered_map>

#include "common.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "timer/timer_task.h"

namespace yuan::net::http 
{
    enum class SessionItemType : char
    {
        invalid = -1,
        ival,
        sz_val,
        dval,
        sval,
        pval
    };

    struct SessionItem
    {
        SessionItemType type = SessionItemType::invalid;
        union 
        {
            int     ival;
            std::size_t sz_val;
            double  dval;
            void    *pval;
        } 
        number;
        std::string sval;
    };

    class HttpSessionContext;

    class HttpSession : public timer::TimerTask
    {
    public:
        HttpSession(uint64_t id, HttpSessionContext *context, timer::TimerManager *timer_manager);
        ~HttpSession() override;
        
        void add_session_value(const std::string &key, int ival);
        void add_session_value(const std::string &key, std::size_t sz);
        void add_session_value(const std::string &key, double dval);
        void add_session_value(const std::string &key, void *pval);
        void add_session_value(const std::string &key, const std::string &sval);

        SessionItem * get_session_value(const std::string &key);

        const std::unordered_map<std::string, SessionItem> & get_session_values();

        void remove_session_value(const std::string &key)
        {
            session_items_.erase(key);
        }

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

        void set_close_call_back(close_callback ccb)
        {
            close_cb_ = ccb;
        }

    public:
        void on_timer(timer::Timer *timer) override;

    public:
        void reset_timer();

    private:
        uint64_t session_id_;
        std::unordered_map<std::string, SessionItem> session_items_;
        HttpSessionContext *context_;
        timer::TimerManager *timer_manager_;
        timer::Timer *conn_timer_;
        close_callback close_cb_;
    };
}

#endif
