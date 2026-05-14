#include "rtsp_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace yuan::net::rtsp
{

namespace
{

std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool parse_int(const std::string &text, int &out)
{
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool RtspParser::parse_request(const std::string &raw, RtspRequest &out_request)
{
    std::istringstream iss(raw);
    std::string line;
    if (!std::getline(iss, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream start(line);
    std::string method_text;
    RtspRequest req;
    if (!(start >> method_text >> req.uri >> req.version)) {
        return false;
    }
    req.method = parse_method(method_text);
    if (req.method == RtspMethod::unknown) {
        return false;
    }
    if (req.version != kRtspVersion) {
        return false;
    }

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }

        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            return false;
        }
        const std::string name = trim(line.substr(0, pos));
        const std::string value = trim(line.substr(pos + 1));
        if (name.empty()) {
            return false;
        }
        req.headers[name] = value;
    }

    std::string body;
    if (std::getline(iss, body, '\0')) {
        req.body = body;
    }

    const std::string *cseq_text = req.header("CSeq");
    if (!cseq_text) {
        return false;
    }
    if (!parse_int(*cseq_text, req.cseq) || req.cseq < 0) {
        return false;
    }

    out_request = std::move(req);
    return true;
}

std::string RtspParser::serialize_response(const RtspResponse &response)
{
    std::ostringstream oss;
    oss << response.version << ' ' << static_cast<int>(response.status) << ' '
        << status_code_reason(response.status) << "\r\n";

    if (response.cseq >= 0) {
        oss << "CSeq: " << response.cseq << "\r\n";
    }

    for (const auto &entry : response.headers) {
        if (entry.first == "CSeq") {
            continue;
        }
        oss << entry.first << ": " << entry.second << "\r\n";
    }

    if (!response.body.empty() && response.headers.find("Content-Length") == response.headers.end()) {
        oss << "Content-Length: " << response.body.size() << "\r\n";
    }

    oss << "\r\n";
    oss << response.body;
    return oss.str();
}

} // namespace yuan::net::rtsp
