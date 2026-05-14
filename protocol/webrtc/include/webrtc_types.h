#ifndef __NET_WEBRTC_TYPES_H__
#define __NET_WEBRTC_TYPES_H__

#include <cstdint>
#include <string>

namespace yuan::net::webrtc
{

enum class SdpType
{
    offer,
    answer,
    pranswer,
    rollback,
};

struct SessionDescription
{
    SdpType type = SdpType::offer;
    std::string sdp;
};

struct IceCandidate
{
    std::string candidate;
    std::string mid;
    int32_t mline_index = -1;
    std::string foundation;
    uint32_t component = 1;
    std::string transport;
    uint32_t priority = 0;
    std::string ip;
    uint16_t port = 0;
    std::string type;
    std::string related_address;
    uint16_t related_port = 0;
};

enum class SignalingState
{
    new_,
    have_local_offer,
    have_remote_offer,
    stable,
};

enum class IceTransportState
{
    new_,
    checking,
    connected,
    failed,
};

enum class IceGatheringState
{
    new_,
    gathering,
    complete,
};

enum class IceChecklistState
{
    idle,
    running,
    completed,
    failed,
};

enum class IceNominationState
{
    none,
    in_progress,
    nominated,
    failed,
};

enum class IceSelectedPairReason
{
    none,
    highest_priority,
    nominated_by_provider,
    forced_by_signal,
};

enum class StunTransactionState
{
    new_,
    request_sent,
    response_received,
    timed_out,
    failed,
};

enum class DtlsTransportState
{
    new_,
    connecting,
    connected,
    failed,
};

enum class PeerConnectionState
{
    new_,
    connecting,
    connected,
    failed,
};

enum class SecurityErrorCode
{
    none,
    dtls_fingerprint_mismatch,
    external_security_error,
};

} // namespace yuan::net::webrtc

#endif
