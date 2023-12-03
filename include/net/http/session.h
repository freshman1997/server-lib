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
        fval,
        pval,
    };

    struct SessionItem
    {
        SessionItemType type = SessionItemType::invalid;
        union 
        {
            int     ival;
            double  fval;
            void  * pval;
        };
    };

    class HttpSession
    {
    public:
        HttpSession(int id);
        

    private:
        int session_id_;
        std::unordered_map<std::string, SessionItem> session_items;
    };
}

#endif
