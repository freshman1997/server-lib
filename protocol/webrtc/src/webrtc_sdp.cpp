#include "webrtc_sdp.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace
{

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string line;
    std::istringstream iss(text);
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string trim(const std::string &value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool parse_int(const std::string &text, int32_t &out)
{
    try {
        std::size_t consumed = 0;
        const long v = std::stol(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        out = static_cast<int32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_direction(const std::string &attr, ::yuan::net::webrtc::SdpMediaDirection &out)
{
    using Direction = ::yuan::net::webrtc::SdpMediaDirection;
    if (attr == "sendrecv") {
        out = Direction::sendrecv;
        return true;
    }
    if (attr == "sendonly") {
        out = Direction::sendonly;
        return true;
    }
    if (attr == "recvonly") {
        out = Direction::recvonly;
        return true;
    }
    if (attr == "inactive") {
        out = Direction::inactive;
        return true;
    }
    return false;
}

const char *direction_to_text(::yuan::net::webrtc::SdpMediaDirection direction)
{
    using Direction = ::yuan::net::webrtc::SdpMediaDirection;
    switch (direction) {
        case Direction::sendrecv:
            return "sendrecv";
        case Direction::sendonly:
            return "sendonly";
        case Direction::recvonly:
            return "recvonly";
        case Direction::inactive:
            return "inactive";
        default:
            return "sendrecv";
    }
}

} // namespace

namespace yuan::net::webrtc
{

bool WebrtcSdp::parse(const std::string &sdp_text, SdpSession &out_session)
{
    const auto lines = split_lines(sdp_text);
    if (lines.empty()) {
        return false;
    }

    SdpSession session;
    SdpMediaSection *current_media = nullptr;

    for (const auto &line : lines) {
        if (starts_with(line, "v=")) {
            if (line != "v=0") {
                return false;
            }
            continue;
        }

        if (starts_with(line, "o=")) {
            session.origin = line.substr(2);
            continue;
        }

        if (starts_with(line, "s=")) {
            session.session_name = line.substr(2);
            continue;
        }

        if (starts_with(line, "t=")) {
            session.timing = line.substr(2);
            continue;
        }

        if (starts_with(line, "a=group:BUNDLE ")) {
            session.bundle_group = line.substr(std::string("a=group:BUNDLE ").size());
            continue;
        }

        if (starts_with(line, "a=fingerprint:")) {
            const std::string value = line.substr(std::string("a=fingerprint:").size());
            const auto sep = value.find(' ');
            if (sep == std::string::npos) {
                return false;
            }
            session.fingerprint.algorithm = trim(value.substr(0, sep));
            session.fingerprint.value = trim(value.substr(sep + 1));
            session.has_fingerprint = !session.fingerprint.algorithm.empty() && !session.fingerprint.value.empty();
            if (!session.has_fingerprint) {
                return false;
            }
            continue;
        }

        if (starts_with(line, "m=")) {
            std::istringstream mss(line.substr(2));
            SdpMediaSection media;
            int32_t port_i = 0;

            if (!(mss >> media.kind >> port_i >> media.protocol)) {
                return false;
            }
            if (media.kind.empty() || media.protocol.empty() || port_i < 0 || port_i > 65535) {
                return false;
            }
            media.port = static_cast<uint16_t>(port_i);

            std::string pt;
            while (mss >> pt) {
                int32_t payload_type = -1;
                if (!parse_int(pt, payload_type)) {
                    return false;
                }
                media.payload_types.push_back(payload_type);
            }

            session.media_sections.push_back(media);
            current_media = &session.media_sections.back();
            continue;
        }

        if (starts_with(line, "a=") && current_media != nullptr) {
            const std::string attr = line.substr(2);

            if (starts_with(attr, "mid:")) {
                current_media->mid = trim(attr.substr(4));
                continue;
            }

            SdpMediaDirection direction;
            if (parse_direction(attr, direction)) {
                current_media->direction = direction;
                continue;
            }

            if (starts_with(attr, "rtpmap:")) {
                const std::string value = attr.substr(std::string("rtpmap:").size());
                const auto sep = value.find(' ');
                if (sep == std::string::npos) {
                    return false;
                }

                int32_t payload_type = -1;
                if (!parse_int(value.substr(0, sep), payload_type)) {
                    return false;
                }

                const std::string enc = value.substr(sep + 1);
                std::vector<std::string> parts;
                std::string token;
                std::istringstream ess(enc);
                while (std::getline(ess, token, '/')) {
                    parts.push_back(token);
                }
                if (parts.size() < 2) {
                    return false;
                }

                SdpRtpMap map;
                map.payload_type = payload_type;
                map.codec = parts[0];
                if (!parse_int(parts[1], map.clock_rate)) {
                    return false;
                }
                if (parts.size() >= 3) {
                    if (!parse_int(parts[2], map.channels)) {
                        return false;
                    }
                }

                current_media->rtp_maps.push_back(map);
                continue;
            }
        }
    }

    if (session.media_sections.empty()) {
        return false;
    }

    for (const auto &media : session.media_sections) {
        if (media.kind.empty() || media.protocol.empty()) {
            return false;
        }
    }

    out_session = std::move(session);
    return true;
}

bool WebrtcSdp::serialize(const SdpSession &session, std::string &out_sdp_text)
{
    if (session.media_sections.empty()) {
        return false;
    }

    std::ostringstream oss;
    oss << "v=0\r\n";
    oss << "o=" << (session.origin.empty() ? "- 0 0 IN IP4 127.0.0.1" : session.origin) << "\r\n";
    oss << "s=" << (session.session_name.empty() ? "-" : session.session_name) << "\r\n";
    oss << "t=" << (session.timing.empty() ? "0 0" : session.timing) << "\r\n";

    if (!session.bundle_group.empty()) {
        oss << "a=group:BUNDLE " << session.bundle_group << "\r\n";
    }

    if (session.has_fingerprint) {
        if (session.fingerprint.algorithm.empty() || session.fingerprint.value.empty()) {
            return false;
        }
        oss << "a=fingerprint:" << session.fingerprint.algorithm << " " << session.fingerprint.value << "\r\n";
    }

    for (const auto &media : session.media_sections) {
        if (media.kind.empty() || media.protocol.empty()) {
            return false;
        }

        oss << "m=" << media.kind << " " << media.port << " " << media.protocol;
        for (int32_t payload_type : media.payload_types) {
            oss << " " << payload_type;
        }
        oss << "\r\n";

        if (!media.mid.empty()) {
            oss << "a=mid:" << media.mid << "\r\n";
        }
        oss << "a=" << direction_to_text(media.direction) << "\r\n";

        for (const auto &map : media.rtp_maps) {
            if (map.payload_type < 0 || map.codec.empty() || map.clock_rate <= 0) {
                return false;
            }
            oss << "a=rtpmap:" << map.payload_type << " " << map.codec << "/" << map.clock_rate;
            if (map.channels > 1) {
                oss << "/" << map.channels;
            }
            oss << "\r\n";
        }
    }

    out_sdp_text = oss.str();
    return true;
}

} // namespace yuan::net::webrtc
