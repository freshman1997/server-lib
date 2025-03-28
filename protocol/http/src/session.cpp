#include "session.h"
#include "context.h"
#include "ops/option.h"
#include "net/connection/connection.h"

#include <iostream>

namespace yuan::net::http 
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

    void HttpSession::add_session_value(const std::string &key, std::size_t sz)
    {
        SessionItem item;
        item.type = SessionItemType::sz_val;
        item.number.sz_val = sz;
        session_items_[key] = item;
    }

    void HttpSession::add_session_value(const std::string &key, double dval)
    {
        SessionItem item;
        item.type = SessionItemType::dval;
        item.number.dval = dval;
        session_items_[key] = item;
    }

    void HttpSession::add_session_value(const std::string &key, const std::string &sval)
    {
        session_items_[key] = { SessionItemType::sval, 0, sval };
    }

    void HttpSession::add_session_value(const std::string &key, void *pval)
    {
        SessionItem item;
        item.type = SessionItemType::pval;
        item.number.pval = pval;
        session_items_[key] = item;
    }

    SessionItem * HttpSession::get_session_value(const std::string &key)
    {
        auto it = session_items_.find(key);
        if (it == session_items_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const std::unordered_map<std::string, SessionItem> & HttpSession::get_session_values()
    {
        return session_items_;
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