#include "string_cmd.h"
#include <cstdlib>

namespace yuan::redis
{
    int StringCmd::unpack(const unsigned char *buffer, const unsigned char *buffer_end)
    {
        if (buffer_end - buffer < 1 || *buffer != value_->get_type())
        {
            return -1;
        }

        ++buffer;

        std::string str_value;
        while (buffer < buffer_end && *buffer != '\r')
        {
            str_value += static_cast<char>(*buffer);
            ++buffer;
        }

        int len = std::atoi(str_value.c_str());
        if (len < 0)
        {
            value_->set_value("");
            return buffer - buffer_end;;
        }

        str_value.clear();
        buffer += 2; // skip \r\n
        
        while (buffer < buffer_end && len > 0)
        {
            str_value += static_cast<char>(*buffer);
            ++buffer;
            --len;
        }

        if (len != 0)
        {
            return -1;
        }

        if (buffer + 2 > buffer_end)
        {
            return -1;
        }
        
        buffer += 2; // skip \r\n

        value_->set_value(str_value);

        return buffer - buffer_end;
    }
}