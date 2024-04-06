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
        
        conn_timer_ = nullptr;
        if (config::close_idle_connection) {
            conn_timer_ = timer_manager_->timeout(config::connection_idle_timeout, this);
        }

        close_cb_ = nullptr;
    }

    HttpSession::~HttpSession()
    {
        if (close_cb_) {
            close_cb_(this);
            close_cb_ = nullptr;
        }

        if (context_) {
            delete context_;
        }
        
        if (conn_timer_) {
            conn_timer_->cancel();
        }
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

    void HttpSession::add_session_value(const std::string &key, void *pval)
    {
        session_items_[key] = { SessionItemType::pval, {.pval = pval}, {} };
    }

    SessionItem * HttpSession::get_session_value(const std::string &key)
    {
        auto it = session_items_.find(key);
        if (it == session_items_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void HttpSession::on_timer(timer::Timer *timer)
    {
        std::cout << "connection idle timeout !!\n";
        context_->get_connection()->close();
    }

    void HttpSession::reset_timer()
    {
        if (config::close_idle_connection) {
            conn_timer_->cancel();
            conn_timer_ = timer_manager_->timeout(config::connection_idle_timeout, this);
        }
    }
}