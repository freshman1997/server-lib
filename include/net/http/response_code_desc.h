#ifndef __RESPONSE_CODE_DESC_H__
#define __RESPONSE_CODE_DESC_H__
#include <string>
#include <unordered_map>
#include "net/http/response_code.h"

extern std::unordered_map<net::http::response_code::ResponseCode, std::string> responseCodeDescs;

#endif