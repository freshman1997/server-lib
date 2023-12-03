#ifndef __RESPONSE_H__
#define __RESPONSE_H__
#include "response_code.h"

namespace net::http
{
    class HttpResponse
    {
    public:

    private:
        response_code::ResponseCode code = response_code::ResponseCode::invalid;
    };
}
#endif
