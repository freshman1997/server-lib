#ifndef __SESSION_H__
#define __SESSION_H__
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

#include "common.h"
#include "coroutine/runtime.h"
#include "timer/timer.h"

namespace yuan::net::http
{
    enum class SessionItemType : char {
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
            int ival;
            std::size_t sz_val;
            double dval;
            void *pval;
        } number;
        std::string sval;
        std::function<void(void *)> pval_deleter;

        SessionItem()
        {
            number.pval = nullptr;
        }

        ~SessionItem()
        {
            destroy();
        }

        SessionItem(const SessionItem &other)
            : type(other.type), sval(other.sval), pval_deleter(other.pval_deleter)
        {
            copy_union(other);
        }

        SessionItem(SessionItem &&other) noexcept
            : type(other.type),
              sval(std::move(other.sval)),
              pval_deleter(std::move(other.pval_deleter))
        {
            move_union(std::move(other));
        }

        SessionItem &operator=(const SessionItem &other)
        {
            if (this != &other) {
                destroy();
                type = other.type;
                sval = other.sval;
                pval_deleter = other.pval_deleter;
                copy_union(other);
            }
            return *this;
        }

        SessionItem &operator=(SessionItem &&other) noexcept
        {
            if (this != &other) {
                destroy();
                type = other.type;
                sval = std::move(other.sval);
                pval_deleter = std::move(other.pval_deleter);
                move_union(std::move(other));
            }
            return *this;
        }

    private:
        void destroy()
        {
            if (type == SessionItemType::pval && number.pval) {
                if (pval_deleter) {
                    pval_deleter(number.pval);
                }
                number.pval = nullptr;
            }
            type = SessionItemType::invalid;
        }

        void copy_union(const SessionItem &other)
        {
            switch (other.type) {
            case SessionItemType::ival:
                number.ival = other.number.ival;
                break;
            case SessionItemType::sz_val:
                number.sz_val = other.number.sz_val;
                break;
            case SessionItemType::dval:
                number.dval = other.number.dval;
                break;
            case SessionItemType::sval:
                break;
            case SessionItemType::pval:
                number.pval = other.number.pval;
                break;
            default:
                number.pval = nullptr;
                break;
            }
        }

        void move_union(SessionItem &&other)
        {
            switch (other.type) {
            case SessionItemType::ival:
                number.ival = other.number.ival;
                break;
            case SessionItemType::sz_val:
                number.sz_val = other.number.sz_val;
                break;
            case SessionItemType::dval:
                number.dval = other.number.dval;
                break;
            case SessionItemType::sval:
                break;
            case SessionItemType::pval:
                number.pval = other.number.pval;
                other.number.pval = nullptr;
                other.type = SessionItemType::invalid;
                break;
            default:
                number.pval = nullptr;
                break;
            }
        }
    };

    class HttpSessionContext;

    class HttpSession
    {
    public:
        HttpSession(uint64_t id, std::unique_ptr<HttpSessionContext> context, coroutine::RuntimeView runtime);
        ~HttpSession();

        HttpSession(const HttpSession &) = delete;
        HttpSession &operator=(const HttpSession &) = delete;

        void add_session_value(const std::string &key, int ival);
        void add_session_value(const std::string &key, std::size_t sz);
        void add_session_value(const std::string &key, double dval);
        void add_session_value(const std::string &key, void *pval, std::function<void(void *)> deleter = nullptr);
        void add_session_value(const std::string &key, const std::string &sval);

        SessionItem *get_session_value(const std::string &key);

        const std::unordered_map<std::string, SessionItem> &get_session_values();

        void remove_session_value(const std::string &key)
        {
            auto it = session_items_.find(key);
            if (it != session_items_.end()) {
                session_items_.erase(it);
            }
        }

        uint64_t get_session_id() const
        {
            return session_id_;
        }

        HttpSessionContext *get_context()
        {
            return context_ ? &*context_ : nullptr;
        }

        void set_close_call_back(close_callback ccb)
        {
            close_cb_ = ccb;
        }

    public:
        void on_idle_timeout();

    public:
        void reset_timer();

    private:
        uint64_t session_id_;
        std::unordered_map<std::string, SessionItem> session_items_;
        std::unique_ptr<HttpSessionContext> context_;
        coroutine::RuntimeView runtime_;
        timer::Timer *conn_timer_;
        close_callback close_cb_;
        bool alive_ = true;
    };
}

#endif
