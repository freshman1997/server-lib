#include "int_cmd.h"
#include <memory>

namespace yuan::redis 
{
    IntCmd::IntCmd()
    {
        value_ = std::make_shared<IntValue>();
    }

    int IntCmd::unpack(const unsigned char *begin, const unsigned char *end)
    {
        if (end - begin < 1 || *begin != value_->get_type())
        {
            return -1;
        }

        const unsigned char *ptr = begin + 1;
        std::string int_str;
        while (ptr < end && *ptr != '\r')
        {
            int_str += static_cast<char>(*ptr);
            ++ptr;
        }

        if (ptr == end || *ptr != '\n')
        {
            return -1;
        }

        ++ptr;

        try { value_->set_value(std::stoll(int_str)); }
        catch (const std::exception &ignore) 
        { return -1; }
        
        return ptr - begin;
    }
}