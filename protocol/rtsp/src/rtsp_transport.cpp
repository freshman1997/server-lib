#include "rtsp_transport.h"

#include <sstream>
#include <string_view>
#include <vector>

namespace yuan::net::rtsp
{

namespace
{

std::string trim(std::string value)
{
    std::size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool parse_pair(const std::string &text, int &left, int &right)
{
    const auto dash = text.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    try {
        left = std::stoi(text.substr(0, dash));
        right = std::stoi(text.substr(dash + 1));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool parse_transport_header(const std::string &header, RtspTransportSpec &out_spec)
{
    std::vector<RtspTransportSpec> specs;
    if (!parse_transport_candidates(header, specs) || specs.empty()) {
        return false;
    }
    out_spec = specs.front();
    return true;
}

bool parse_transport_candidates(const std::string &header, std::vector<RtspTransportSpec> &out_specs)
{
    out_specs.clear();

    std::string candidate;
    std::istringstream header_stream(header);
    while (std::getline(header_stream, candidate, ',')) {
        const std::string piece = trim(candidate);
        if (piece.empty()) {
            continue;
        }

        RtspTransportSpec spec;
        std::istringstream iss(piece);
        std::string token;
        bool saw_proto = false;
        bool valid = true;

        while (std::getline(iss, token, ';')) {
            token = trim(token);
            if (token.empty()) {
                continue;
            }

            if (!saw_proto) {
                saw_proto = true;
                if (token == "RTP/AVP/TCP") {
                    spec.transport = RtspLowerTransport::rtp_avp_tcp;
                } else if (token == "RTP/AVP") {
                    spec.transport = RtspLowerTransport::rtp_avp_udp;
                } else {
                    valid = false;
                }
                if (!valid) {
                    break;
                }
                continue;
            }

            if (token == "unicast") {
                spec.unicast = true;
                continue;
            }
            if (token == "multicast") {
                spec.unicast = false;
                continue;
            }

            constexpr std::string_view kClientPort = "client_port=";
            constexpr std::string_view kServerPort = "server_port=";
            constexpr std::string_view kInterleaved = "interleaved=";

            if (token.rfind(kClientPort, 0) == 0) {
                int a = -1;
                int b = -1;
                if (!parse_pair(token.substr(kClientPort.size()), a, b)) {
                    valid = false;
                    break;
                }
                spec.client_rtp_port = a;
                spec.client_rtcp_port = b;
                continue;
            }
            if (token.rfind(kServerPort, 0) == 0) {
                int a = -1;
                int b = -1;
                if (!parse_pair(token.substr(kServerPort.size()), a, b)) {
                    valid = false;
                    break;
                }
                spec.server_rtp_port = a;
                spec.server_rtcp_port = b;
                continue;
            }
            if (token.rfind(kInterleaved, 0) == 0) {
                int a = -1;
                int b = -1;
                if (!parse_pair(token.substr(kInterleaved.size()), a, b)) {
                    valid = false;
                    break;
                }
                spec.interleaved_rtp_channel = a;
                spec.interleaved_rtcp_channel = b;
                continue;
            }
        }

        if (!valid || !saw_proto || spec.transport == RtspLowerTransport::unknown) {
            continue;
        }
        out_specs.push_back(spec);
    }

    return !out_specs.empty();
}

std::string format_transport_header(const RtspTransportSpec &spec)
{
    std::ostringstream oss;
    if (spec.transport == RtspLowerTransport::rtp_avp_tcp) {
        oss << "RTP/AVP/TCP";
    } else if (spec.transport == RtspLowerTransport::rtp_avp_udp) {
        oss << "RTP/AVP";
    } else {
        return {};
    }

    oss << ';' << (spec.unicast ? "unicast" : "multicast");

    if (spec.client_rtp_port >= 0 && spec.client_rtcp_port >= 0) {
        oss << ";client_port=" << spec.client_rtp_port << '-' << spec.client_rtcp_port;
    }
    if (spec.server_rtp_port >= 0 && spec.server_rtcp_port >= 0) {
        oss << ";server_port=" << spec.server_rtp_port << '-' << spec.server_rtcp_port;
    }
    if (spec.interleaved_rtp_channel >= 0 && spec.interleaved_rtcp_channel >= 0) {
        oss << ";interleaved=" << spec.interleaved_rtp_channel << '-' << spec.interleaved_rtcp_channel;
    }

    return oss.str();
}

} // namespace yuan::net::rtsp
