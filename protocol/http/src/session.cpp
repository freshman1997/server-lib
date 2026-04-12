#include "session.h"
#include "context.h"
#include "ops/option.h"
#include "net/connection/connection.h"
#include "timer/timer_util.hpp"

namespace yuan::net::http 
{
    HttpSession::HttpSession(uint64_t id, HttpSessionContext *context, timer::TimerManager *timer_manager)
        : session_id_(id), context_(context), timer_manager_(timer_manager)
        , conn_timer_(nullptr), close_cb_(nullptr)
    {
        context_->set_session(this);

        if (config::close_idle_connection && timer_manager_) {
            conn_timer_ = timer::TimerUtil::build_timeout_timer(
                timer_manager_, config::connection_idle_timeout, this, &HttpSession::on_timer);
        }
    }

    HttpSession::~HttpSession()
    {
        // 先调用关闭回调（清理文件句柄等资源）
        if (close_cb_) {
            close_cb_(this);
            close_cb_ = nullptr;
        }

        // 取消空闲定时器
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        // 清理所有 pval 类型资源（ifstream 等）
        for (auto &item : session_items_) {
            if (item.second.type == SessionItemType::pval && item.second.number.pval) {
                item.second.number.pval = nullptr;  // 外部已通过 close_cb 释放
            }
            item.second.type = SessionItemType::invalid;
        }
        session_items_.clear();

        // 最后删除 context
        if (context_) {
            delete context_;
            context_ = nullptr;
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
        session_items_[key] = { SessionItemType::sval, {}, sval };
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
        if (context_ && context_->get_connection()) {
            context_->get_connection()->close();
        }
    }

    void HttpSession::reset_timer()
    {
        if (!conn_timer_ || !timer_manager_ || !config::close_idle_connection) {
            return;
        }
        conn_timer_->cancel();
        conn_timer_ = timer::TimerUtil::build_timeout_timer(
            timer_manager_, config::connection_idle_timeout, this, &HttpSession::on_timer);
    }
}
