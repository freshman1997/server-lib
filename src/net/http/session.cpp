#include "net/http/session.h"
#include "net/http/context.h"

namespace net::http 
{
    HttpSession::HttpSession(uint64_t id, HttpSessionContext *context) : session_id_(id), context_(context)
    {
        context_->set_session(this);
    }

    HttpSession::~HttpSession()
    {
        if (context_) {
            delete context_;
        }
        context_ = nullptr;
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
}