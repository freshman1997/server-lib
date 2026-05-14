#include "rtsp_sdp.h"

#include <sstream>

namespace yuan::net::rtsp
{

bool parse_sdp(const std::string &text, RtspSdpDescription &out_description)
{
    std::istringstream iss(text);
    std::string line;
    RtspSdpDescription desc;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("s=", 0) == 0) {
            desc.session_name = line.substr(2);
            continue;
        }

        if (line.rfind("m=", 0) == 0) {
            std::istringstream mss(line.substr(2));
            RtspSdpMedia media;
            int port = 0;
            std::string proto;
            if (!(mss >> media.kind >> port >> proto >> media.payload_type)) {
                return false;
            }
            desc.media.push_back(std::move(media));
            continue;
        }

        if (line.rfind("a=rtpmap:", 0) == 0 && !desc.media.empty()) {
            const std::string map = line.substr(9);
            const auto sep = map.find(' ');
            if (sep == std::string::npos) {
                return false;
            }

            const std::string payload = map.substr(0, sep);
            const std::string codec_clock = map.substr(sep + 1);
            const auto slash = codec_clock.find('/');
            if (slash == std::string::npos) {
                return false;
            }

            int payload_type = -1;
            int clock = 0;
            try {
                payload_type = std::stoi(payload);
                clock = std::stoi(codec_clock.substr(slash + 1));
            } catch (...) {
                return false;
            }

            auto &media = desc.media.back();
            media.payload_type = payload_type;
            media.codec = codec_clock.substr(0, slash);
            media.clock_rate = clock;
        }
    }

    if (desc.media.empty()) {
        return false;
    }

    out_description = std::move(desc);
    return true;
}

bool serialize_sdp(const RtspSdpDescription &description, std::string &out_text)
{
    if (description.media.empty()) {
        return false;
    }

    std::ostringstream oss;
    oss << "v=0\r\n";
    oss << "o=- 0 0 IN IP4 127.0.0.1\r\n";
    oss << "s=" << (description.session_name.empty() ? "rtsp-stream" : description.session_name) << "\r\n";
    oss << "t=0 0\r\n";

    for (const auto &media : description.media) {
        if (media.kind.empty() || media.payload_type < 0) {
            return false;
        }
        oss << "m=" << media.kind << " 0 RTP/AVP " << media.payload_type << "\r\n";
        if (!media.codec.empty() && media.clock_rate > 0) {
            oss << "a=rtpmap:" << media.payload_type << ' ' << media.codec << '/' << media.clock_rate << "\r\n";
        }
    }

    out_text = oss.str();
    return true;
}

} // namespace yuan::net::rtsp
