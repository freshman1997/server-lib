#include "int_cmd.h"

namespace yuan::redis 
{
    int IntCmd::on_reply(const unsigned char *begin, const unsigned char *end)
    {
        return 0;
    }
}