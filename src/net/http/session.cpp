#include "net/http/session.h"
#include "net/http/context.h"
#include "net/http/ops/option.h"
#include "net/base/connection/connection.h"

#include <iostream>

namespace net::http 
{
    HttpSession::HttpSession(uint64_t id, HttpSessionContext *context, timer::TimerManager *timer_manager) : session_id_(id), context_(context), timer_manager_(timer_manager)
    {
        context_->set_session(this);
        conn_timer_ = timer_manager_->timeout(config::connection_idle_timeout, this);
    }

    HttpSession::~HttpSession()
    {
        if (context_) {
            delete context_;
        }
        conn_timer_->cancel();
    }

    void HttpSession::add_session_value(const std::string &key, int ival)
    {
        session_items_[key] = { SessionItemType::ival, ival, {} };
    }

    void HttpSession::add_session_value(const std::string &key, double dval)
    {
        session_items_[key] = { SessionItemType::dval, {.dval = dval}, {} };
    }

    void HttpSession::add_session_value(const std::string &key, const std::string &sval)
    {
        session_items_[key] = { SessionItemType::sval, 0, sval };
    }

    void HttpSession::on_timer(timer::Timer *timer)
    {
        std::cout << "connection idle timeout !!\n";
        context_->get_connection()->abort();
    }

    void HttpSession::reset_timer()
    {
        conn_timer_->cancel();
        conn_timer_ = timer_manager_->timeout(config::connection_idle_timeout, this);
    }
}