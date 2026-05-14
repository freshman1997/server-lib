#ifndef __NET_WEBRTC_SECURITY_UTILS_H__
#define __NET_WEBRTC_SECURITY_UTILS_H__

#include "webrtc_types.h"

#include <string>

namespace yuan::net::webrtc
{

const char *to_security_error_text(SecurityErrorCode code);
bool parse_security_error_text(const std::string &text, SecurityErrorCode &out_code);
SecurityErrorCode infer_security_error_code(const std::string &error_text);

} // namespace yuan::net::webrtc

#endif
