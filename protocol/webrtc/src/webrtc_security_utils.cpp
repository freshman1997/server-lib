#include "webrtc_security_utils.h"

namespace yuan::net::webrtc
{

const char *to_security_error_text(SecurityErrorCode code)
{
    switch (code) {
        case SecurityErrorCode::none:
            return "";
        case SecurityErrorCode::dtls_fingerprint_mismatch:
            return "dtls_fingerprint_mismatch";
        case SecurityErrorCode::external_security_error:
            return "external_security_error";
        default:
            return "external_security_error";
    }
}

bool parse_security_error_text(const std::string &text, SecurityErrorCode &out_code)
{
    if (text.empty()) {
        out_code = SecurityErrorCode::none;
        return true;
    }
    if (text == "dtls_fingerprint_mismatch") {
        out_code = SecurityErrorCode::dtls_fingerprint_mismatch;
        return true;
    }
    if (text == "external_security_error") {
        out_code = SecurityErrorCode::external_security_error;
        return true;
    }
    return false;
}

SecurityErrorCode infer_security_error_code(const std::string &error_text)
{
    SecurityErrorCode code = SecurityErrorCode::none;
    if (parse_security_error_text(error_text, code)) {
        return code;
    }
    return error_text.empty() ? SecurityErrorCode::none : SecurityErrorCode::external_security_error;
}

} // namespace yuan::net::webrtc
