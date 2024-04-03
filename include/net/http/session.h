#ifndef __SESSION_H__
#define __SESSION_H__
#include <string>
#include <unordered_map>

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

    class HttpSession
    {
    public:
        HttpSession(uint64_t id, HttpSessionContext *context);
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

    private:
        uint64_t session_id_;
        std::unordered_map<std::string, SessionItem> session_items_;
        HttpSessionContext *context_;
    };
}

#endif
