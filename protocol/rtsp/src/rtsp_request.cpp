#include "rtsp_request.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::rtsp
{

namespace
{

bool iequals(const std::string &lhs, const std::string &rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

const std::string *RtspRequest::header(const std::string &name) const
{
    auto direct = headers.find(name);
    if (direct != headers.end()) {
        return &direct->second;
    }

    for (const auto &entry : headers) {
        if (iequals(entry.first, name)) {
            return &entry.second;
        }
    }
    return nullptr;
}

} // namespace yuan::net::rtsp
