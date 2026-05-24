#include "session.h"
#include "base/time.h"
#include "context.h"
#include "ops/option.h"
#include "net/connection/connection.h"

#include <algorithm>

namespace yuan::net::http
{
    HttpSession::HttpSession(uint64_t id,
                             std::unique_ptr<HttpSessionContext> context,
                             coroutine::RuntimeView runtime,
                             bool enable_idle_timer)
        : session_id_(id), context_(std::move(context)), runtime_(runtime), close_cb_(nullptr)
    {
        context_->set_session(this);

        if (enable_idle_timer && config::close_idle_connection && config::connection_idle_timeout > 0) {
            idle_timer_enabled_ = true;
            last_active_ms_ = yuan::base::time::steady_now_ms();
            schedule_idle_timer(static_cast<uint32_t>(config::connection_idle_timeout));
        }
    }

    HttpSession::~HttpSession()
    {
        alive_ = false;

        if (close_cb_) {
            close_cb_(this);
            close_cb_ = nullptr;
        }

        if (conn_timer_) {
            conn_timer_.cancel();
            conn_timer_.reset();
        }

        session_items_.clear();

        context_.reset();
    }

    void HttpSession::add_session_value(const std::string & key, int ival)
    {
        SessionItem item;
        item.type = SessionItemType::ival;
        item.number.ival = ival;
        session_items_[key] = std::move(item);
    }

    void HttpSession::add_session_value(const std::string & key, std::size_t sz)
    {
        SessionItem item;
        item.type = SessionItemType::sz_val;
        item.number.sz_val = sz;
        session_items_[key] = item;
    }

    void HttpSession::add_session_value(const std::string & key, double dval)
    {
        SessionItem item;
        item.type = SessionItemType::dval;
        item.number.dval = dval;
        session_items_[key] = item;
    }

    void HttpSession::add_session_value(const std::string & key, const std::string & sval)
    {
        SessionItem item;
        item.type = SessionItemType::sval;
        item.sval = sval;
        session_items_[key] = std::move(item);
    }

    void HttpSession::add_session_value(const std::string & key, void * pval, std::function<void(void *)> deleter)
    {
        SessionItem item;
        item.type = SessionItemType::pval;
        item.number.pval = pval;
        item.pval_deleter = std::move(deleter);
        session_items_[key] = std::move(item);
    }

    SessionItem *HttpSession::get_session_value(const std::string & key)
    {
        auto it = session_items_.find(key);
        if (it == session_items_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    const std::unordered_map<std::string, SessionItem> &HttpSession::get_session_values()
    {
        return session_items_;
    }

    void HttpSession::on_idle_timeout()
    {
        if (!alive_) {
            return;
        }

        if (idle_timer_enabled_ && config::close_idle_connection && config::connection_idle_timeout > 0) {
            const auto now_ms = yuan::base::time::steady_now_ms();
            const auto timeout_ms = static_cast<uint64_t>(config::connection_idle_timeout);
            const auto elapsed_ms = now_ms >= last_active_ms_ ? now_ms - last_active_ms_ : timeout_ms;
            if (elapsed_ms < timeout_ms) {
                const auto remaining_ms = static_cast<uint32_t>((std::max<uint64_t>)(1, timeout_ms - elapsed_ms));
                schedule_idle_timer(remaining_ms);
                return;
            }
        }

        if (context_ && context_->get_connection()) {
            context_->get_connection()->close();
        }
    }

    void HttpSession::reset_timer()
    {
        if (!alive_ || !idle_timer_enabled_ || !config::close_idle_connection || config::connection_idle_timeout <= 0) {
            return;
        }
        last_active_ms_ = yuan::base::time::steady_now_ms();
        if (!conn_timer_) {
            schedule_idle_timer(static_cast<uint32_t>(config::connection_idle_timeout));
        }
    }

    void HttpSession::schedule_idle_timer(uint32_t timeout_ms)
    {
        if (!runtime_.timer_manager() || timeout_ms == 0) {
            return;
        }
        conn_timer_ = runtime_.schedule(timeout_ms, [this]() { on_idle_timeout(); });
    }
}
