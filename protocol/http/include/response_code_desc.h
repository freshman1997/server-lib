#ifndef __RESPONSE_CODE_DESC_H__
#define __RESPONSE_CODE_DESC_H__
#include <string>
#include <unordered_map>
#include "response_code.h"

namespace yuan::net::http 
{
    extern std::unordered_map<ResponseCode, std::string> responseCodeDescs;
}

#endif