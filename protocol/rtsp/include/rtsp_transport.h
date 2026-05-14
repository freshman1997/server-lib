#ifndef __NET_RTSP_TRANSPORT_H__
#define __NET_RTSP_TRANSPORT_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::rtsp
{

enum class RtspLowerTransport
{
    rtp_avp_tcp,
    rtp_avp_udp,
    unknown,
};

struct RtspTransportSpec
{
    RtspLowerTransport transport = RtspLowerTransport::unknown;
    int client_rtp_port = -1;
    int client_rtcp_port = -1;
    int server_rtp_port = -1;
    int server_rtcp_port = -1;
    int interleaved_rtp_channel = -1;
    int interleaved_rtcp_channel = -1;
    bool unicast = true;
};

bool parse_transport_header(const std::string &header, RtspTransportSpec &out_spec);
bool parse_transport_candidates(const std::string &header, std::vector<RtspTransportSpec> &out_specs);
std::string format_transport_header(const RtspTransportSpec &spec);

} // namespace yuan::net::rtsp

#endif
