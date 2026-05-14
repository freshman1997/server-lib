#include "rtsp_framing.h"

#include <cctype>
#include <sstream>

namespace yuan::net::rtsp
{

namespace
{

bool parse_content_length(const std::string &headers, std::size_t &out_length)
{
    out_length = 0;
    std::istringstream iss(headers);
    std::string line;
    if (!std::getline(iss, line)) {
        return true;
    }

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) {
            name.pop_back();
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.erase(value.begin());
        }

        for (char &ch : name) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (name != "content-length") {
            continue;
        }

        try {
            std::size_t consumed = 0;
            const std::size_t parsed = static_cast<std::size_t>(std::stoul(value, &consumed, 10));
            if (consumed != value.size()) {
                return false;
            }
            out_length = parsed;
            return true;
        } catch (...) {
            return false;
        }
    }

    return true;
}

} // namespace

void RtspStreamFramer::push(std::string_view bytes)
{
    buffer_.append(bytes.data(), bytes.size());
}

RtspFrameParseResult RtspStreamFramer::pop(RtspFrame &out_frame)
{
    if (buffer_.empty()) {
        return RtspFrameParseResult::need_more;
    }

    if (buffer_[0] == '$') {
        if (buffer_.size() < 4) {
            return RtspFrameParseResult::need_more;
        }
        const uint8_t channel = static_cast<uint8_t>(buffer_[1]);
        const uint16_t payload_length =
            static_cast<uint16_t>((static_cast<uint8_t>(buffer_[2]) << 8) |
                                  static_cast<uint8_t>(buffer_[3]));
        const std::size_t frame_size = static_cast<std::size_t>(4 + payload_length);
        if (buffer_.size() < frame_size) {
            return RtspFrameParseResult::need_more;
        }

        out_frame.kind = RtspFrameKind::interleaved;
        out_frame.channel = channel;
        out_frame.data = buffer_.substr(4, payload_length);
        buffer_.erase(0, frame_size);
        return RtspFrameParseResult::ok;
    }

    const auto header_end = buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return RtspFrameParseResult::need_more;
    }

    const std::string headers = buffer_.substr(0, header_end + 4);
    std::size_t content_length = 0;
    if (!parse_content_length(headers, content_length)) {
        return RtspFrameParseResult::malformed;
    }

    const std::size_t frame_size = header_end + 4 + content_length;
    if (buffer_.size() < frame_size) {
        return RtspFrameParseResult::need_more;
    }

    out_frame.kind = RtspFrameKind::rtsp_request;
    out_frame.channel = 0;
    out_frame.data = buffer_.substr(0, frame_size);
    buffer_.erase(0, frame_size);
    return RtspFrameParseResult::ok;
}

void RtspStreamFramer::clear()
{
    buffer_.clear();
}

} // namespace yuan::net::rtsp
