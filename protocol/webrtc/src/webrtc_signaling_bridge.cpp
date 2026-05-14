#include "webrtc_signaling_bridge.h"
#include "webrtc_ice_transport.h"
#include "webrtc_security_utils.h"

#include <nlohmann/json.hpp>
#include <map>
#include <utility>

namespace
{

using Json = nlohmann::json;

const char *to_sdp_type_text(::yuan::net::webrtc::SdpType type)
{
    switch (type) {
        case ::yuan::net::webrtc::SdpType::offer:
            return "offer";
        case ::yuan::net::webrtc::SdpType::answer:
            return "answer";
        case ::yuan::net::webrtc::SdpType::pranswer:
            return "pranswer";
        case ::yuan::net::webrtc::SdpType::rollback:
            return "rollback";
        default:
            return "offer";
    }
}

bool parse_sdp_type_text(const std::string &text, ::yuan::net::webrtc::SdpType &out_type)
{
    if (text == "offer") {
        out_type = ::yuan::net::webrtc::SdpType::offer;
        return true;
    }
    if (text == "answer") {
        out_type = ::yuan::net::webrtc::SdpType::answer;
        return true;
    }
    if (text == "pranswer") {
        out_type = ::yuan::net::webrtc::SdpType::pranswer;
        return true;
    }
    if (text == "rollback") {
        out_type = ::yuan::net::webrtc::SdpType::rollback;
        return true;
    }
    return false;
}

const char *to_kind_text(::yuan::net::webrtc::WebrtcSignalingBridge::SignalingMessage::Kind kind)
{
    using Kind = ::yuan::net::webrtc::WebrtcSignalingBridge::SignalingMessage::Kind;
    switch (kind) {
        case Kind::offer:
            return "offer";
        case Kind::answer:
            return "answer";
        case Kind::candidate:
            return "candidate";
        case Kind::rollback:
            return "rollback";
        case Kind::ice_state:
            return "ice_state";
        case Kind::ice_nomination:
            return "ice_nomination";
        case Kind::diagnostics:
            return "diagnostics";
        case Kind::dtls_state:
            return "dtls_state";
        case Kind::security_state:
            return "security_state";
        default:
            return "unknown";
    }
}

::yuan::net::webrtc::WebrtcSignalingBridge::SignalingMessage::Kind parse_kind_text(const std::string &text)
{
    using Kind = ::yuan::net::webrtc::WebrtcSignalingBridge::SignalingMessage::Kind;
    if (text == "offer") {
        return Kind::offer;
    }
    if (text == "answer") {
        return Kind::answer;
    }
    if (text == "candidate") {
        return Kind::candidate;
    }
    if (text == "rollback") {
        return Kind::rollback;
    }
    if (text == "ice_state") {
        return Kind::ice_state;
    }
    if (text == "ice_nomination") {
        return Kind::ice_nomination;
    }
    if (text == "diagnostics") {
        return Kind::diagnostics;
    }
    if (text == "dtls_state") {
        return Kind::dtls_state;
    }
    if (text == "security_state") {
        return Kind::security_state;
    }
    return Kind::unknown;
}

const char *to_ice_state_text(::yuan::net::webrtc::IceTransportState state)
{
    switch (state) {
        case ::yuan::net::webrtc::IceTransportState::new_:
            return "new";
        case ::yuan::net::webrtc::IceTransportState::checking:
            return "checking";
        case ::yuan::net::webrtc::IceTransportState::connected:
            return "connected";
        case ::yuan::net::webrtc::IceTransportState::failed:
            return "failed";
        default:
            return "new";
    }
}

bool parse_ice_state_text(const std::string &text, ::yuan::net::webrtc::IceTransportState &out_state)
{
    if (text == "new") {
        out_state = ::yuan::net::webrtc::IceTransportState::new_;
        return true;
    }
    if (text == "checking") {
        out_state = ::yuan::net::webrtc::IceTransportState::checking;
        return true;
    }
    if (text == "connected") {
        out_state = ::yuan::net::webrtc::IceTransportState::connected;
        return true;
    }
    if (text == "failed") {
        out_state = ::yuan::net::webrtc::IceTransportState::failed;
        return true;
    }
    return false;
}

const char *to_ice_nomination_state_text(::yuan::net::webrtc::IceNominationState state)
{
    switch (state) {
        case ::yuan::net::webrtc::IceNominationState::none:
            return "none";
        case ::yuan::net::webrtc::IceNominationState::in_progress:
            return "in_progress";
        case ::yuan::net::webrtc::IceNominationState::nominated:
            return "nominated";
        case ::yuan::net::webrtc::IceNominationState::failed:
            return "failed";
        default:
            return "none";
    }
}

bool parse_ice_nomination_state_text(const std::string &text, ::yuan::net::webrtc::IceNominationState &out_state)
{
    if (text == "none") {
        out_state = ::yuan::net::webrtc::IceNominationState::none;
        return true;
    }
    if (text == "in_progress") {
        out_state = ::yuan::net::webrtc::IceNominationState::in_progress;
        return true;
    }
    if (text == "nominated") {
        out_state = ::yuan::net::webrtc::IceNominationState::nominated;
        return true;
    }
    if (text == "failed") {
        out_state = ::yuan::net::webrtc::IceNominationState::failed;
        return true;
    }
    return false;
}

const char *to_ice_selected_pair_reason_text(::yuan::net::webrtc::IceSelectedPairReason reason)
{
    switch (reason) {
        case ::yuan::net::webrtc::IceSelectedPairReason::none:
            return "none";
        case ::yuan::net::webrtc::IceSelectedPairReason::highest_priority:
            return "highest_priority";
        case ::yuan::net::webrtc::IceSelectedPairReason::nominated_by_provider:
            return "nominated_by_provider";
        case ::yuan::net::webrtc::IceSelectedPairReason::forced_by_signal:
            return "forced_by_signal";
        default:
            return "none";
    }
}

bool parse_ice_selected_pair_reason_text(const std::string &text, ::yuan::net::webrtc::IceSelectedPairReason &out_reason)
{
    if (text == "none") {
        out_reason = ::yuan::net::webrtc::IceSelectedPairReason::none;
        return true;
    }
    if (text == "highest_priority") {
        out_reason = ::yuan::net::webrtc::IceSelectedPairReason::highest_priority;
        return true;
    }
    if (text == "nominated_by_provider") {
        out_reason = ::yuan::net::webrtc::IceSelectedPairReason::nominated_by_provider;
        return true;
    }
    if (text == "forced_by_signal") {
        out_reason = ::yuan::net::webrtc::IceSelectedPairReason::forced_by_signal;
        return true;
    }
    return false;
}

const char *to_stun_transaction_state_text(::yuan::net::webrtc::StunTransactionState state)
{
    switch (state) {
        case ::yuan::net::webrtc::StunTransactionState::new_:
            return "new";
        case ::yuan::net::webrtc::StunTransactionState::request_sent:
            return "request_sent";
        case ::yuan::net::webrtc::StunTransactionState::response_received:
            return "response_received";
        case ::yuan::net::webrtc::StunTransactionState::timed_out:
            return "timed_out";
        case ::yuan::net::webrtc::StunTransactionState::failed:
            return "failed";
        default:
            return "new";
    }
}

bool parse_stun_transaction_state_text(const std::string &text, ::yuan::net::webrtc::StunTransactionState &out)
{
    if (text == "new") {
        out = ::yuan::net::webrtc::StunTransactionState::new_;
        return true;
    }
    if (text == "request_sent") {
        out = ::yuan::net::webrtc::StunTransactionState::request_sent;
        return true;
    }
    if (text == "response_received") {
        out = ::yuan::net::webrtc::StunTransactionState::response_received;
        return true;
    }
    if (text == "timed_out") {
        out = ::yuan::net::webrtc::StunTransactionState::timed_out;
        return true;
    }
    if (text == "failed") {
        out = ::yuan::net::webrtc::StunTransactionState::failed;
        return true;
    }
    return false;
}

const char *to_dtls_state_text(::yuan::net::webrtc::DtlsTransportState state)
{
    switch (state) {
        case ::yuan::net::webrtc::DtlsTransportState::new_:
            return "new";
        case ::yuan::net::webrtc::DtlsTransportState::connecting:
            return "connecting";
        case ::yuan::net::webrtc::DtlsTransportState::connected:
            return "connected";
        case ::yuan::net::webrtc::DtlsTransportState::failed:
            return "failed";
        default:
            return "new";
    }
}

bool parse_dtls_state_text(const std::string &text, ::yuan::net::webrtc::DtlsTransportState &out_state)
{
    if (text == "new") {
        out_state = ::yuan::net::webrtc::DtlsTransportState::new_;
        return true;
    }
    if (text == "connecting") {
        out_state = ::yuan::net::webrtc::DtlsTransportState::connecting;
        return true;
    }
    if (text == "connected") {
        out_state = ::yuan::net::webrtc::DtlsTransportState::connected;
        return true;
    }
    if (text == "failed") {
        out_state = ::yuan::net::webrtc::DtlsTransportState::failed;
        return true;
    }
    return false;
}

bool has_common_payload_type(const ::yuan::net::webrtc::SdpMediaSection &offer_media,
                             const ::yuan::net::webrtc::SdpMediaSection &answer_media)
{
    for (int32_t offer_pt : offer_media.payload_types) {
        for (int32_t answer_pt : answer_media.payload_types) {
            if (offer_pt == answer_pt) {
                return true;
            }
        }
    }
    return false;
}

bool is_payload_mapping_compatible(const ::yuan::net::webrtc::SdpMediaSection &offer_media,
                                   const ::yuan::net::webrtc::SdpMediaSection &answer_media)
{
    std::map<int32_t, ::yuan::net::webrtc::SdpRtpMap> offer_maps;
    for (const auto &map : offer_media.rtp_maps) {
        offer_maps[map.payload_type] = map;
    }

    std::map<int32_t, ::yuan::net::webrtc::SdpRtpMap> answer_maps;
    for (const auto &map : answer_media.rtp_maps) {
        answer_maps[map.payload_type] = map;
    }

    for (int32_t answer_pt : answer_media.payload_types) {
        auto offer_it = offer_maps.find(answer_pt);
        if (offer_it == offer_maps.end()) {
            continue;
        }
        auto answer_it = answer_maps.find(answer_pt);
        if (answer_it == answer_maps.end()) {
            continue;
        }

        const auto &offer_map = offer_it->second;
        const auto &answer_map = answer_it->second;
        if (offer_map.codec != answer_map.codec
            || offer_map.clock_rate != answer_map.clock_rate
            || offer_map.channels != answer_map.channels) {
            return false;
        }
    }

    return true;
}

bool validate_answer_against_offer(const ::yuan::net::webrtc::SdpSession &offer,
                                   const ::yuan::net::webrtc::SdpSession &answer,
                                   const char *media_count_error,
                                   const char *media_kind_error,
                                   const char *mid_error,
                                   const char *payload_error,
                                   std::string &out_error)
{
    if (offer.media_sections.size() != answer.media_sections.size()) {
        out_error = media_count_error;
        return false;
    }

    for (std::size_t i = 0; i < offer.media_sections.size(); ++i) {
        const auto &offer_media = offer.media_sections[i];
        const auto &answer_media = answer.media_sections[i];

        if (offer_media.kind != answer_media.kind) {
            out_error = media_kind_error;
            return false;
        }

        if (offer_media.mid != answer_media.mid) {
            out_error = mid_error;
            return false;
        }

        if (!has_common_payload_type(offer_media, answer_media)) {
            out_error = payload_error;
            return false;
        }

        if (!is_payload_mapping_compatible(offer_media, answer_media)) {
            out_error = payload_error;
            return false;
        }
    }

    out_error.clear();
    return true;
}

} // namespace

namespace yuan::net::webrtc
{

bool WebrtcSignalingBridge::set_local_description(const SessionDescription &description)
{
    if (description.sdp.empty()) {
        last_sdp_error_ = kErrEmptySdp;
        return false;
    }

    if (!is_valid_local_transition(description.type)) {
        last_sdp_error_ = kErrInvalidStateTransition;
        return false;
    }

    SdpSession parsed_sdp;
    if (!WebrtcSdp::parse(description.sdp, parsed_sdp)) {
        last_sdp_error_ = kErrSdpParseFailed;
        return false;
    }

    std::string negotiation_error;
    if (!validate_local_negotiation(description.type, parsed_sdp, negotiation_error)) {
        last_sdp_error_ = negotiation_error;
        return false;
    }

    local_description_ = description;
    has_local_description_ = true;
    parsed_local_sdp_ = std::move(parsed_sdp);
    has_parsed_local_sdp_ = true;

    switch (description.type) {
        case SdpType::offer:
            signaling_state_ = SignalingState::have_local_offer;
            break;
        case SdpType::answer:
        case SdpType::pranswer:
            signaling_state_ = SignalingState::stable;
            break;
        case SdpType::rollback:
            signaling_state_ = SignalingState::stable;
            break;
    }

    last_sdp_error_.clear();

    return true;
}

bool WebrtcSignalingBridge::set_remote_description(const SessionDescription &description)
{
    if (description.sdp.empty()) {
        last_sdp_error_ = kErrEmptySdp;
        return false;
    }

    if (!is_valid_remote_transition(description.type)) {
        last_sdp_error_ = kErrInvalidStateTransition;
        return false;
    }

    SdpSession parsed_sdp;
    if (!WebrtcSdp::parse(description.sdp, parsed_sdp)) {
        last_sdp_error_ = kErrSdpParseFailed;
        return false;
    }

    std::string negotiation_error;
    if (!validate_remote_negotiation(description.type, parsed_sdp, negotiation_error)) {
        last_sdp_error_ = negotiation_error;
        return false;
    }

    remote_description_ = description;
    has_remote_description_ = true;
    parsed_remote_sdp_ = std::move(parsed_sdp);
    has_parsed_remote_sdp_ = true;

    switch (description.type) {
        case SdpType::offer:
            signaling_state_ = SignalingState::have_remote_offer;
            break;
        case SdpType::answer:
        case SdpType::pranswer:
            signaling_state_ = SignalingState::stable;
            break;
        case SdpType::rollback:
            signaling_state_ = SignalingState::stable;
            break;
    }

    last_sdp_error_.clear();

    return true;
}

bool WebrtcSignalingBridge::add_remote_candidate(const IceCandidate &candidate)
{
    if (candidate.candidate.empty()) {
        return false;
    }
    remote_candidates_.push_back(candidate);
    return true;
}

bool WebrtcSignalingBridge::apply_signaling_message(const SignalingMessage &message, bool as_remote)
{
    switch (message.kind) {
        case SignalingMessage::Kind::offer:
        case SignalingMessage::Kind::answer:
        case SignalingMessage::Kind::rollback:
            return as_remote ? set_remote_description(message.description) : set_local_description(message.description);
        case SignalingMessage::Kind::candidate:
            return add_remote_candidate(message.candidate);
        case SignalingMessage::Kind::ice_state:
        case SignalingMessage::Kind::ice_nomination:
        case SignalingMessage::Kind::diagnostics:
        case SignalingMessage::Kind::dtls_state:
        case SignalingMessage::Kind::security_state:
            return true;
        case SignalingMessage::Kind::unknown:
        default:
            return false;
    }
}

bool WebrtcSignalingBridge::parse_signaling_message(const std::string &json_text, SignalingMessage &out_message) const
{
    try {
        const Json root = Json::parse(json_text);
        if (!root.is_object() || !root.contains("type") || !root["type"].is_string()) {
            return false;
        }

        const std::string type = root["type"].get<std::string>();
        out_message = SignalingMessage{};
        out_message.kind = parse_kind_text(type);
        if (out_message.kind == SignalingMessage::Kind::unknown) {
            return false;
        }

        if (out_message.kind == SignalingMessage::Kind::candidate) {
            if (!root.contains("candidate") || !root["candidate"].is_string()) {
                return false;
            }
            out_message.candidate.candidate = root["candidate"].get<std::string>();
            (void) parse_ice_candidate_sdp(out_message.candidate.candidate, out_message.candidate);
            if (root.contains("mid") && root["mid"].is_string()) {
                out_message.candidate.mid = root["mid"].get<std::string>();
            }
            if (root.contains("mline_index") && root["mline_index"].is_number_integer()) {
                out_message.candidate.mline_index = root["mline_index"].get<int32_t>();
            }
            if (root.contains("foundation") && root["foundation"].is_string()) {
                const std::string foundation = root["foundation"].get<std::string>();
                if (!foundation.empty()) {
                    out_message.candidate.foundation = foundation;
                }
            }
            if (root.contains("component") && root["component"].is_number_integer()) {
                const uint32_t component = root["component"].get<uint32_t>();
                if (component != 0) {
                    out_message.candidate.component = component;
                }
            }
            if (root.contains("transport") && root["transport"].is_string()) {
                const std::string transport = root["transport"].get<std::string>();
                if (!transport.empty()) {
                    out_message.candidate.transport = transport;
                }
            }
            if (root.contains("priority") && root["priority"].is_number_integer()) {
                const uint32_t priority = root["priority"].get<uint32_t>();
                if (priority != 0) {
                    out_message.candidate.priority = priority;
                }
            }
            if (root.contains("ip") && root["ip"].is_string()) {
                const std::string ip = root["ip"].get<std::string>();
                if (!ip.empty()) {
                    out_message.candidate.ip = ip;
                }
            }
            if (root.contains("port") && root["port"].is_number_integer()) {
                const uint16_t port = root["port"].get<uint16_t>();
                if (port != 0) {
                    out_message.candidate.port = port;
                }
            }
            if (root.contains("candidate_type") && root["candidate_type"].is_string()) {
                const std::string type = root["candidate_type"].get<std::string>();
                if (!type.empty()) {
                    out_message.candidate.type = type;
                }
            }
            if (root.contains("related_address") && root["related_address"].is_string()) {
                const std::string related_address = root["related_address"].get<std::string>();
                if (!related_address.empty()) {
                    out_message.candidate.related_address = related_address;
                }
            }
            if (root.contains("related_port") && root["related_port"].is_number_integer()) {
                const uint16_t related_port = root["related_port"].get<uint16_t>();
                if (related_port != 0) {
                    out_message.candidate.related_port = related_port;
                }
            }
            return true;
        }

        if (out_message.kind == SignalingMessage::Kind::ice_state) {
            if (!root.contains("state") || !root["state"].is_string()) {
                return false;
            }
            return parse_ice_state_text(root["state"].get<std::string>(), out_message.ice_transport_state);
        }

        if (out_message.kind == SignalingMessage::Kind::ice_nomination) {
            if (!root.contains("state") || !root["state"].is_string()) {
                return false;
            }
            if (!parse_ice_nomination_state_text(root["state"].get<std::string>(), out_message.ice_nomination_state)) {
                return false;
            }
            out_message.selected_pair_reason = IceSelectedPairReason::none;
            out_message.selected_pair_reason_text.clear();
            out_message.nomination_transaction_id.clear();
            if (root.contains("selected_pair_reason") && root["selected_pair_reason"].is_string()) {
                if (!parse_ice_selected_pair_reason_text(
                        root["selected_pair_reason"].get<std::string>(),
                        out_message.selected_pair_reason)) {
                    return false;
                }
            }
            if (root.contains("selected_pair_reason_text") && root["selected_pair_reason_text"].is_string()) {
                out_message.selected_pair_reason_text = root["selected_pair_reason_text"].get<std::string>();
            }
            if (out_message.selected_pair_reason_text.empty()) {
                out_message.selected_pair_reason_text = to_ice_selected_pair_reason_text(out_message.selected_pair_reason);
            }
            if (root.contains("nomination_transaction_id") && root["nomination_transaction_id"].is_string()) {
                out_message.nomination_transaction_id = root["nomination_transaction_id"].get<std::string>();
            }
            return true;
        }

        if (out_message.kind == SignalingMessage::Kind::diagnostics) {
            out_message.diagnostics_scope = "peer_session";
            out_message.diagnostics_schema_version = "v2";
            bool schema_version_explicit_v2 = false;
            bool schema_version_explicit_v1 = false;
            const bool parser_release_mode_strict_v2 = diagnostics_emission_config_.release_mode_strict_v2;
            out_message.diagnostics_emit_flat_compat_fields = parser_release_mode_strict_v2
                                                                  ? false
                                                                  : diagnostics_emission_config_.emit_flat_compat_fields;
            out_message.diagnostics_rollout_alert_window_seconds =
                diagnostics_emission_config_.rollout_alert_window_seconds;
            out_message.diagnostics_rollout_mismatch_count_alert_threshold =
                diagnostics_emission_config_.rollout_mismatch_count_alert_threshold;
            out_message.diagnostics_rollout_mismatch_ratio_threshold_ppm =
                diagnostics_emission_config_.rollout_mismatch_ratio_threshold_ppm;
            out_message.diagnostics_rollout_current_mismatch_ratio_ppm = 0;
            out_message.diagnostics_rollout_alert_active = false;
            out_message.diagnostics_rollout_progress_blocked = false;
            out_message.diagnostics_rollout_ready_for_progress = true;
            if (root.contains("scope") && root["scope"].is_string()) {
                out_message.diagnostics_scope = root["scope"].get<std::string>();
            }
            if (root.contains("schema_version")) {
                if (!root["schema_version"].is_string()) {
                    return false;
                }
                out_message.diagnostics_schema_version = root["schema_version"].get<std::string>();
                if (out_message.diagnostics_schema_version != "v1"
                    && out_message.diagnostics_schema_version != "v2") {
                    return false;
                }
                schema_version_explicit_v2 = out_message.diagnostics_schema_version == "v2";
                schema_version_explicit_v1 = out_message.diagnostics_schema_version == "v1";
            } else {
                const bool has_nested_v2_fields =
                    (root.contains("nomination") && root["nomination"].is_object())
                    || (root.contains("stun") && root["stun"].is_object())
                    || (root.contains("queues") && root["queues"].is_object())
                    || (root.contains("policy") && root["policy"].is_object());
                out_message.diagnostics_schema_version = has_nested_v2_fields ? "v2" : "v1";
            }
            if (root.contains("emit_flat_compat_fields")) {
                if (!root["emit_flat_compat_fields"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_emit_flat_compat_fields =
                    root["emit_flat_compat_fields"].get<bool>();
            }
            if (root.contains("rollout_alert_window_seconds")) {
                if (!root["rollout_alert_window_seconds"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_rollout_alert_window_seconds =
                    root["rollout_alert_window_seconds"].get<uint64_t>();
            }
            if (root.contains("rollout_mismatch_count_alert_threshold")) {
                if (!root["rollout_mismatch_count_alert_threshold"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_rollout_mismatch_count_alert_threshold =
                    root["rollout_mismatch_count_alert_threshold"].get<uint64_t>();
            }
            if (root.contains("rollout_mismatch_ratio_threshold_ppm")) {
                if (!root["rollout_mismatch_ratio_threshold_ppm"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_rollout_mismatch_ratio_threshold_ppm =
                    root["rollout_mismatch_ratio_threshold_ppm"].get<uint64_t>();
            }
            if (root.contains("rollout_current_mismatch_ratio_ppm")) {
                if (!root["rollout_current_mismatch_ratio_ppm"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_rollout_current_mismatch_ratio_ppm =
                    root["rollout_current_mismatch_ratio_ppm"].get<uint64_t>();
            }
            if (root.contains("rollout_alert_active")) {
                if (!root["rollout_alert_active"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_rollout_alert_active =
                    root["rollout_alert_active"].get<bool>();
            }
            if (root.contains("rollout_progress_blocked")) {
                if (!root["rollout_progress_blocked"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_rollout_progress_blocked =
                    root["rollout_progress_blocked"].get<bool>();
            }
            if (root.contains("rollout_ready_for_progress")) {
                if (!root["rollout_ready_for_progress"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_rollout_ready_for_progress =
                    root["rollout_ready_for_progress"].get<bool>();
            }
            if (root.contains("rollout_policy")) {
                if (!root["rollout_policy"].is_object()) {
                    return false;
                }
                const Json &rollout = root["rollout_policy"];
                if (!rollout.contains("alert_window_seconds")
                    || !rollout["alert_window_seconds"].is_number_unsigned()) {
                    return false;
                }
                if (!rollout.contains("mismatch_count_alert_threshold")
                    || !rollout["mismatch_count_alert_threshold"].is_number_unsigned()) {
                    return false;
                }
                if (!rollout.contains("mismatch_ratio_threshold_ppm")
                    || !rollout["mismatch_ratio_threshold_ppm"].is_number_unsigned()) {
                    return false;
                }
                const bool nested_has_current_ratio = rollout.contains("current_mismatch_ratio_ppm");
                if (nested_has_current_ratio
                    && !rollout["current_mismatch_ratio_ppm"].is_number_unsigned()) {
                    return false;
                }
                const bool nested_has_alert_active = rollout.contains("alert_active");
                if (nested_has_alert_active && !rollout["alert_active"].is_boolean()) {
                    return false;
                }
                const bool nested_has_progress_blocked = rollout.contains("progress_blocked");
                if (nested_has_progress_blocked && !rollout["progress_blocked"].is_boolean()) {
                    return false;
                }
                const bool nested_has_ready_for_progress = rollout.contains("ready_for_progress");
                if (nested_has_ready_for_progress && !rollout["ready_for_progress"].is_boolean()) {
                    return false;
                }

                const uint64_t nested_window = rollout["alert_window_seconds"].get<uint64_t>();
                const uint64_t nested_count_threshold =
                    rollout["mismatch_count_alert_threshold"].get<uint64_t>();
                const uint64_t nested_ratio_threshold =
                    rollout["mismatch_ratio_threshold_ppm"].get<uint64_t>();
                const uint64_t nested_current_ratio =
                    nested_has_current_ratio ? rollout["current_mismatch_ratio_ppm"].get<uint64_t>() : 0;
                const bool nested_alert_active =
                    nested_has_alert_active ? rollout["alert_active"].get<bool>() : false;
                const bool nested_progress_blocked =
                    nested_has_progress_blocked ? rollout["progress_blocked"].get<bool>() : false;
                const bool nested_ready_for_progress =
                    nested_has_ready_for_progress ? rollout["ready_for_progress"].get<bool>() : true;
                const bool has_flat_rollout_fields =
                    root.contains("rollout_alert_window_seconds")
                    || root.contains("rollout_mismatch_count_alert_threshold")
                    || root.contains("rollout_mismatch_ratio_threshold_ppm")
                    || root.contains("rollout_current_mismatch_ratio_ppm")
                    || root.contains("rollout_alert_active")
                    || root.contains("rollout_progress_blocked")
                    || root.contains("rollout_ready_for_progress");
                if (has_flat_rollout_fields) {
                    if (nested_window != out_message.diagnostics_rollout_alert_window_seconds
                        || nested_count_threshold
                               != out_message.diagnostics_rollout_mismatch_count_alert_threshold
                        || nested_ratio_threshold
                               != out_message.diagnostics_rollout_mismatch_ratio_threshold_ppm
                        || (nested_has_current_ratio
                            && nested_current_ratio
                                   != out_message.diagnostics_rollout_current_mismatch_ratio_ppm)
                        || (nested_has_alert_active
                            && nested_alert_active != out_message.diagnostics_rollout_alert_active)
                        || (nested_has_progress_blocked
                            && nested_progress_blocked
                                   != out_message.diagnostics_rollout_progress_blocked)
                        || (nested_has_ready_for_progress
                            && nested_ready_for_progress
                                   != out_message.diagnostics_rollout_ready_for_progress)) {
                        return false;
                    }
                }
                out_message.diagnostics_rollout_alert_window_seconds = nested_window;
                out_message.diagnostics_rollout_mismatch_count_alert_threshold = nested_count_threshold;
                out_message.diagnostics_rollout_mismatch_ratio_threshold_ppm = nested_ratio_threshold;
                if (nested_has_current_ratio) {
                    out_message.diagnostics_rollout_current_mismatch_ratio_ppm = nested_current_ratio;
                }
                if (nested_has_alert_active) {
                    out_message.diagnostics_rollout_alert_active = nested_alert_active;
                }
                if (nested_has_progress_blocked) {
                    out_message.diagnostics_rollout_progress_blocked = nested_progress_blocked;
                }
                if (nested_has_ready_for_progress) {
                    out_message.diagnostics_rollout_ready_for_progress = nested_ready_for_progress;
                }
            }
            bool message_release_mode_strict_v2 = parser_release_mode_strict_v2;
            if (root.contains("release_mode_strict_v2")) {
                if (!root["release_mode_strict_v2"].is_boolean()) {
                    return false;
                }
                message_release_mode_strict_v2 = root["release_mode_strict_v2"].get<bool>();
            }
            out_message.diagnostics_release_mode_strict_v2 =
                parser_release_mode_strict_v2 || message_release_mode_strict_v2;
            if (out_message.diagnostics_release_mode_strict_v2) {
                if (out_message.diagnostics_schema_version != "v2") {
                    return false;
                }
                if (out_message.diagnostics_emit_flat_compat_fields) {
                    return false;
                }
            }
            if (!out_message.diagnostics_emit_flat_compat_fields
                && out_message.diagnostics_schema_version == "v1") {
                return false;
            }
            const bool has_nomination_object = root.contains("nomination") && root["nomination"].is_object();
            const bool has_flat_nomination_fields =
                root.contains("ice_nomination_state")
                || root.contains("has_selected_pair")
                || root.contains("selected_pair_reason")
                || root.contains("selected_pair_reason_text");
            if (schema_version_explicit_v2 && !has_nomination_object) {
                return false;
            }
            if (schema_version_explicit_v1 && !has_flat_nomination_fields) {
                return false;
            }
            if (!has_nomination_object && !has_flat_nomination_fields) {
                return false;
            }
            if (has_nomination_object) {
                const Json &nomination = root["nomination"];
                if (!nomination.contains("state") || !nomination["state"].is_string()) {
                    return false;
                }
                if (!parse_ice_nomination_state_text(
                        nomination["state"].get<std::string>(),
                        out_message.diagnostics_ice_nomination_state)) {
                    return false;
                }
                if (!nomination.contains("has_selected_pair") || !nomination["has_selected_pair"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_has_selected_pair = nomination["has_selected_pair"].get<bool>();
                if (!nomination.contains("selected_pair_reason") || !nomination["selected_pair_reason"].is_string()) {
                    return false;
                }
                if (!parse_ice_selected_pair_reason_text(
                        nomination["selected_pair_reason"].get<std::string>(),
                        out_message.diagnostics_selected_pair_reason)) {
                    return false;
                }
                if (!nomination.contains("selected_pair_reason_text") || !nomination["selected_pair_reason_text"].is_string()) {
                    return false;
                }
                out_message.diagnostics_selected_pair_reason_text = nomination["selected_pair_reason_text"].get<std::string>();
            } else {
                if (!root.contains("ice_nomination_state") || !root["ice_nomination_state"].is_string()) {
                    return false;
                }
                if (!parse_ice_nomination_state_text(
                        root["ice_nomination_state"].get<std::string>(),
                        out_message.diagnostics_ice_nomination_state)) {
                    return false;
                }
                if (!root.contains("has_selected_pair") || !root["has_selected_pair"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_has_selected_pair = root["has_selected_pair"].get<bool>();
                if (!root.contains("selected_pair_reason") || !root["selected_pair_reason"].is_string()) {
                    return false;
                }
                if (!parse_ice_selected_pair_reason_text(
                        root["selected_pair_reason"].get<std::string>(),
                        out_message.diagnostics_selected_pair_reason)) {
                    return false;
                }
                if (!root.contains("selected_pair_reason_text") || !root["selected_pair_reason_text"].is_string()) {
                    return false;
                }
                out_message.diagnostics_selected_pair_reason_text = root["selected_pair_reason_text"].get<std::string>();
            }
            if (has_nomination_object && has_flat_nomination_fields) {
                ++diagnostics_v2_flat_duplicate_seen_count_;
                if (!root.contains("ice_nomination_state") || !root["ice_nomination_state"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                IceNominationState flat_state = IceNominationState::none;
                if (!parse_ice_nomination_state_text(root["ice_nomination_state"].get<std::string>(), flat_state)) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("has_selected_pair") || !root["has_selected_pair"].is_boolean()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                const bool flat_has_selected_pair = root["has_selected_pair"].get<bool>();
                if (!root.contains("selected_pair_reason") || !root["selected_pair_reason"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                IceSelectedPairReason flat_reason = IceSelectedPairReason::none;
                if (!parse_ice_selected_pair_reason_text(root["selected_pair_reason"].get<std::string>(), flat_reason)) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("selected_pair_reason_text") || !root["selected_pair_reason_text"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                const std::string flat_reason_text = root["selected_pair_reason_text"].get<std::string>();
                if (flat_state != out_message.diagnostics_ice_nomination_state
                    || flat_has_selected_pair != out_message.diagnostics_has_selected_pair
                    || flat_reason != out_message.diagnostics_selected_pair_reason
                    || flat_reason_text != out_message.diagnostics_selected_pair_reason_text) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
            }

            const bool has_stun_object = root.contains("stun") && root["stun"].is_object();
            const bool has_flat_stun_fields =
                root.contains("last_ice_error")
                || root.contains("stun_transaction_count")
                || root.contains("has_last_stun_transaction")
                || root.contains("last_stun_transaction_id")
                || root.contains("last_stun_transaction_state");
            if (schema_version_explicit_v2 && !has_stun_object) {
                return false;
            }
            if (schema_version_explicit_v1 && !has_flat_stun_fields) {
                return false;
            }
            if (!has_stun_object && !has_flat_stun_fields) {
                return false;
            }
            if (has_stun_object) {
                const Json &stun = root["stun"];
                if (!stun.contains("last_ice_error") || !stun["last_ice_error"].is_string()) {
                    return false;
                }
                out_message.diagnostics_last_ice_error = stun["last_ice_error"].get<std::string>();
                if (!stun.contains("transaction_count") || !stun["transaction_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_stun_transaction_count = stun["transaction_count"].get<uint64_t>();
                if (!stun.contains("has_last_transaction") || !stun["has_last_transaction"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_has_last_stun_transaction = stun["has_last_transaction"].get<bool>();
                if (!stun.contains("last_transaction_id") || !stun["last_transaction_id"].is_string()) {
                    return false;
                }
                out_message.diagnostics_last_stun_transaction_id = stun["last_transaction_id"].get<std::string>();
                if (!stun.contains("last_transaction_state") || !stun["last_transaction_state"].is_string()) {
                    return false;
                }
                if (!parse_stun_transaction_state_text(
                        stun["last_transaction_state"].get<std::string>(),
                        out_message.diagnostics_last_stun_transaction_state)) {
                    return false;
                }
            } else {
                if (!root.contains("last_ice_error") || !root["last_ice_error"].is_string()) {
                    return false;
                }
                out_message.diagnostics_last_ice_error = root["last_ice_error"].get<std::string>();
                if (!root.contains("stun_transaction_count") || !root["stun_transaction_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_stun_transaction_count = root["stun_transaction_count"].get<uint64_t>();
                if (!root.contains("has_last_stun_transaction") || !root["has_last_stun_transaction"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_has_last_stun_transaction = root["has_last_stun_transaction"].get<bool>();
                if (!root.contains("last_stun_transaction_id") || !root["last_stun_transaction_id"].is_string()) {
                    return false;
                }
                out_message.diagnostics_last_stun_transaction_id = root["last_stun_transaction_id"].get<std::string>();
                if (!root.contains("last_stun_transaction_state") || !root["last_stun_transaction_state"].is_string()) {
                    return false;
                }
                if (!parse_stun_transaction_state_text(
                        root["last_stun_transaction_state"].get<std::string>(),
                        out_message.diagnostics_last_stun_transaction_state)) {
                    return false;
                }
            }
            if (has_stun_object && has_flat_stun_fields) {
                ++diagnostics_v2_flat_duplicate_seen_count_;
                if (!root.contains("last_ice_error") || !root["last_ice_error"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("stun_transaction_count") || !root["stun_transaction_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("has_last_stun_transaction") || !root["has_last_stun_transaction"].is_boolean()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("last_stun_transaction_id") || !root["last_stun_transaction_id"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("last_stun_transaction_state") || !root["last_stun_transaction_state"].is_string()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                StunTransactionState flat_state = StunTransactionState::new_;
                if (!parse_stun_transaction_state_text(root["last_stun_transaction_state"].get<std::string>(), flat_state)) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (root["last_ice_error"].get<std::string>() != out_message.diagnostics_last_ice_error
                    || root["stun_transaction_count"].get<uint64_t>() != out_message.diagnostics_stun_transaction_count
                    || root["has_last_stun_transaction"].get<bool>() != out_message.diagnostics_has_last_stun_transaction
                    || root["last_stun_transaction_id"].get<std::string>() != out_message.diagnostics_last_stun_transaction_id
                    || flat_state != out_message.diagnostics_last_stun_transaction_state) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
            }
            const bool has_queues_object = root.contains("queues") && root["queues"].is_object();
            const bool has_flat_queue_fields =
                root.contains("pending_nomination_signal_count")
                || root.contains("dropped_nomination_signal_count")
                || root.contains("dropped_nomination_signal_overflow_count")
                || root.contains("dropped_nomination_signal_trim_count")
                || root.contains("pending_diagnostics_signal_count")
                || root.contains("dropped_diagnostics_signal_count")
                || root.contains("dropped_diagnostics_signal_overflow_count")
                || root.contains("dropped_diagnostics_signal_trim_count");

            if (schema_version_explicit_v2 && !has_queues_object) {
                return false;
            }
            if (schema_version_explicit_v1 && !has_flat_queue_fields) {
                return false;
            }
            if (!has_queues_object && !has_flat_queue_fields) {
                return false;
            }

            if (has_queues_object) {
                const Json &queues = root["queues"];
                if (!queues.contains("nomination") || !queues["nomination"].is_object()) {
                    return false;
                }
                if (!queues.contains("diagnostics") || !queues["diagnostics"].is_object()) {
                    return false;
                }
                const Json &nomination = queues["nomination"];
                const Json &diagnostics = queues["diagnostics"];

                if (!nomination.contains("pending_signal_count")
                    || !nomination["pending_signal_count"].is_number_unsigned()) {
                    return false;
                }
                if (!nomination.contains("dropped_signal_count")
                    || !nomination["dropped_signal_count"].is_number_unsigned()) {
                    return false;
                }
                if (!nomination.contains("dropped_signal_overflow_count")
                    || !nomination["dropped_signal_overflow_count"].is_number_unsigned()) {
                    return false;
                }
                if (!nomination.contains("dropped_signal_trim_count")
                    || !nomination["dropped_signal_trim_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_pending_nomination_signal_count =
                    nomination["pending_signal_count"].get<uint64_t>();
                out_message.diagnostics_dropped_nomination_signal_count =
                    nomination["dropped_signal_count"].get<uint64_t>();
                out_message.diagnostics_dropped_nomination_signal_overflow_count =
                    nomination["dropped_signal_overflow_count"].get<uint64_t>();
                out_message.diagnostics_dropped_nomination_signal_trim_count =
                    nomination["dropped_signal_trim_count"].get<uint64_t>();

                if (!diagnostics.contains("pending_signal_count")
                    || !diagnostics["pending_signal_count"].is_number_unsigned()) {
                    return false;
                }
                if (!diagnostics.contains("dropped_signal_count")
                    || !diagnostics["dropped_signal_count"].is_number_unsigned()) {
                    return false;
                }
                if (!diagnostics.contains("dropped_signal_overflow_count")
                    || !diagnostics["dropped_signal_overflow_count"].is_number_unsigned()) {
                    return false;
                }
                if (!diagnostics.contains("dropped_signal_trim_count")
                    || !diagnostics["dropped_signal_trim_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_pending_diagnostics_signal_count =
                    diagnostics["pending_signal_count"].get<uint64_t>();
                out_message.diagnostics_dropped_diagnostics_signal_count =
                    diagnostics["dropped_signal_count"].get<uint64_t>();
                out_message.diagnostics_dropped_diagnostics_signal_overflow_count =
                    diagnostics["dropped_signal_overflow_count"].get<uint64_t>();
                out_message.diagnostics_dropped_diagnostics_signal_trim_count =
                    diagnostics["dropped_signal_trim_count"].get<uint64_t>();
            } else {
                if (!root.contains("pending_nomination_signal_count")
                    || !root["pending_nomination_signal_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_pending_nomination_signal_count =
                    root["pending_nomination_signal_count"].get<uint64_t>();
                if (!root.contains("dropped_nomination_signal_count")
                    || !root["dropped_nomination_signal_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_nomination_signal_count =
                    root["dropped_nomination_signal_count"].get<uint64_t>();
                if (!root.contains("dropped_nomination_signal_overflow_count")
                    || !root["dropped_nomination_signal_overflow_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_nomination_signal_overflow_count =
                    root["dropped_nomination_signal_overflow_count"].get<uint64_t>();
                if (!root.contains("dropped_nomination_signal_trim_count")
                    || !root["dropped_nomination_signal_trim_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_nomination_signal_trim_count =
                    root["dropped_nomination_signal_trim_count"].get<uint64_t>();
                if (!root.contains("pending_diagnostics_signal_count")
                    || !root["pending_diagnostics_signal_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_pending_diagnostics_signal_count =
                    root["pending_diagnostics_signal_count"].get<uint64_t>();
                if (!root.contains("dropped_diagnostics_signal_count")
                    || !root["dropped_diagnostics_signal_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_diagnostics_signal_count =
                    root["dropped_diagnostics_signal_count"].get<uint64_t>();
                if (!root.contains("dropped_diagnostics_signal_overflow_count")
                    || !root["dropped_diagnostics_signal_overflow_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_diagnostics_signal_overflow_count =
                    root["dropped_diagnostics_signal_overflow_count"].get<uint64_t>();
                if (!root.contains("dropped_diagnostics_signal_trim_count")
                    || !root["dropped_diagnostics_signal_trim_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_dropped_diagnostics_signal_trim_count =
                    root["dropped_diagnostics_signal_trim_count"].get<uint64_t>();
            }
            const bool has_migration_object = root.contains("migration") && root["migration"].is_object();
            const bool has_flat_migration_fields =
                root.contains("v2_flat_duplicate_seen_count")
                || root.contains("v2_flat_duplicate_mismatch_count");
            out_message.diagnostics_v2_flat_duplicate_seen_count = 0;
            out_message.diagnostics_v2_flat_duplicate_mismatch_count = 0;

            if (has_migration_object) {
                const Json &migration = root["migration"];
                if (!migration.contains("v2_flat_duplicate_seen_count")
                    || !migration["v2_flat_duplicate_seen_count"].is_number_unsigned()) {
                    return false;
                }
                if (!migration.contains("v2_flat_duplicate_mismatch_count")
                    || !migration["v2_flat_duplicate_mismatch_count"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_v2_flat_duplicate_seen_count =
                    migration["v2_flat_duplicate_seen_count"].get<uint64_t>();
                out_message.diagnostics_v2_flat_duplicate_mismatch_count =
                    migration["v2_flat_duplicate_mismatch_count"].get<uint64_t>();
            }

            if (has_flat_migration_fields) {
                if (!root.contains("v2_flat_duplicate_seen_count")
                    || !root["v2_flat_duplicate_seen_count"].is_number_unsigned()) {
                    return false;
                }
                if (!root.contains("v2_flat_duplicate_mismatch_count")
                    || !root["v2_flat_duplicate_mismatch_count"].is_number_unsigned()) {
                    return false;
                }
                const uint64_t flat_seen = root["v2_flat_duplicate_seen_count"].get<uint64_t>();
                const uint64_t flat_mismatch = root["v2_flat_duplicate_mismatch_count"].get<uint64_t>();
                if (has_migration_object) {
                    if (flat_seen != out_message.diagnostics_v2_flat_duplicate_seen_count
                        || flat_mismatch != out_message.diagnostics_v2_flat_duplicate_mismatch_count) {
                        return false;
                    }
                } else {
                    out_message.diagnostics_v2_flat_duplicate_seen_count = flat_seen;
                    out_message.diagnostics_v2_flat_duplicate_mismatch_count = flat_mismatch;
                }
            }
            if (has_queues_object && has_flat_queue_fields) {
                ++diagnostics_v2_flat_duplicate_seen_count_;
                if (!root.contains("pending_nomination_signal_count")
                    || !root["pending_nomination_signal_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_nomination_signal_count")
                    || !root["dropped_nomination_signal_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_nomination_signal_overflow_count")
                    || !root["dropped_nomination_signal_overflow_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_nomination_signal_trim_count")
                    || !root["dropped_nomination_signal_trim_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("pending_diagnostics_signal_count")
                    || !root["pending_diagnostics_signal_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_diagnostics_signal_count")
                    || !root["dropped_diagnostics_signal_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_diagnostics_signal_overflow_count")
                    || !root["dropped_diagnostics_signal_overflow_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("dropped_diagnostics_signal_trim_count")
                    || !root["dropped_diagnostics_signal_trim_count"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (root["pending_nomination_signal_count"].get<uint64_t>()
                        != out_message.diagnostics_pending_nomination_signal_count
                    || root["dropped_nomination_signal_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_nomination_signal_count
                    || root["dropped_nomination_signal_overflow_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_nomination_signal_overflow_count
                    || root["dropped_nomination_signal_trim_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_nomination_signal_trim_count
                    || root["pending_diagnostics_signal_count"].get<uint64_t>()
                           != out_message.diagnostics_pending_diagnostics_signal_count
                    || root["dropped_diagnostics_signal_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_diagnostics_signal_count
                    || root["dropped_diagnostics_signal_overflow_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_diagnostics_signal_overflow_count
                    || root["dropped_diagnostics_signal_trim_count"].get<uint64_t>()
                           != out_message.diagnostics_dropped_diagnostics_signal_trim_count) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
            }
            const bool has_policy_object = root.contains("policy") && root["policy"].is_object();
            const bool has_flat_policy_fields =
                root.contains("policy_keep_latest_only")
                || root.contains("policy_max_pending_signals")
                || root.contains("policy_nomination_max_pending_signals");

            if (schema_version_explicit_v2 && !has_policy_object) {
                return false;
            }
            if (schema_version_explicit_v1 && !has_flat_policy_fields) {
                return false;
            }
            if (!has_policy_object && !has_flat_policy_fields) {
                return false;
            }

            if (has_policy_object) {
                const Json &policy = root["policy"];
                if (!policy.contains("keep_latest_only") || !policy["keep_latest_only"].is_boolean()) {
                    return false;
                }
                if (!policy.contains("max_pending_signals") || !policy["max_pending_signals"].is_number_unsigned()) {
                    return false;
                }
                if (!policy.contains("nomination_max_pending_signals")
                    || !policy["nomination_max_pending_signals"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_policy_keep_latest_only = policy["keep_latest_only"].get<bool>();
                out_message.diagnostics_policy_max_pending_signals = policy["max_pending_signals"].get<uint64_t>();
                out_message.diagnostics_policy_nomination_max_pending_signals =
                    policy["nomination_max_pending_signals"].get<uint64_t>();
            } else {
                if (!root.contains("policy_keep_latest_only")
                    || !root["policy_keep_latest_only"].is_boolean()) {
                    return false;
                }
                out_message.diagnostics_policy_keep_latest_only =
                    root["policy_keep_latest_only"].get<bool>();
                if (!root.contains("policy_max_pending_signals")
                    || !root["policy_max_pending_signals"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_policy_max_pending_signals =
                    root["policy_max_pending_signals"].get<uint64_t>();
                if (!root.contains("policy_nomination_max_pending_signals")
                    || !root["policy_nomination_max_pending_signals"].is_number_unsigned()) {
                    return false;
                }
                out_message.diagnostics_policy_nomination_max_pending_signals =
                    root["policy_nomination_max_pending_signals"].get<uint64_t>();
            }
            if (has_policy_object && has_flat_policy_fields) {
                ++diagnostics_v2_flat_duplicate_seen_count_;
                if (!root.contains("policy_keep_latest_only")
                    || !root["policy_keep_latest_only"].is_boolean()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("policy_max_pending_signals")
                    || !root["policy_max_pending_signals"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (!root.contains("policy_nomination_max_pending_signals")
                    || !root["policy_nomination_max_pending_signals"].is_number_unsigned()) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
                if (root["policy_keep_latest_only"].get<bool>() != out_message.diagnostics_policy_keep_latest_only
                    || root["policy_max_pending_signals"].get<uint64_t>() != out_message.diagnostics_policy_max_pending_signals
                    || root["policy_nomination_max_pending_signals"].get<uint64_t>()
                           != out_message.diagnostics_policy_nomination_max_pending_signals) {
                    ++diagnostics_v2_flat_duplicate_mismatch_count_;
                    return false;
                }
            }
            return true;
        }

        if (out_message.kind == SignalingMessage::Kind::dtls_state) {
            if (!root.contains("state") || !root["state"].is_string()) {
                return false;
            }
            return parse_dtls_state_text(root["state"].get<std::string>(), out_message.dtls_transport_state);
        }

        if (out_message.kind == SignalingMessage::Kind::security_state) {
            out_message.security_error_code = SecurityErrorCode::none;
            out_message.security_error.clear();

            const bool has_security_code = root.contains("security_code") && root["security_code"].is_string();
            const bool has_security_error = root.contains("security_error") && root["security_error"].is_string();
            if (!has_security_code && !has_security_error) {
                return false;
            }

            if (has_security_code) {
                const std::string code_text = root["security_code"].get<std::string>();
                if (!parse_security_error_text(code_text, out_message.security_error_code)) {
                    return false;
                }
            }

            if (has_security_error) {
                out_message.security_error = root["security_error"].get<std::string>();
            }

            if (!has_security_code) {
                if (!parse_security_error_text(out_message.security_error, out_message.security_error_code)) {
                    out_message.security_error_code = SecurityErrorCode::external_security_error;
                }
            }

            if (has_security_code) {
                if (out_message.security_error_code == SecurityErrorCode::external_security_error) {
                    if (out_message.security_error.empty()) {
                        out_message.security_error = to_security_error_text(out_message.security_error_code);
                    }
                } else {
                    out_message.security_error = to_security_error_text(out_message.security_error_code);
                }
            } else if (out_message.security_error.empty()) {
                out_message.security_error = to_security_error_text(out_message.security_error_code);
            }
            return true;
        }

        if (!root.contains("sdp") || !root["sdp"].is_string()) {
            return false;
        }
        out_message.description.sdp = root["sdp"].get<std::string>();
        return parse_sdp_type_text(type, out_message.description.type);
    } catch (...) {
        return false;
    }
}

std::string WebrtcSignalingBridge::to_signaling_json(const SignalingMessage &message) const
{
    Json root;
    root["type"] = to_kind_text(message.kind);

    switch (message.kind) {
        case SignalingMessage::Kind::offer:
        case SignalingMessage::Kind::answer:
        case SignalingMessage::Kind::rollback:
            root["sdp"] = message.description.sdp;
            break;
        case SignalingMessage::Kind::candidate:
            if (message.candidate.candidate.empty()
                && !message.candidate.foundation.empty()
                && !message.candidate.transport.empty()
                && !message.candidate.ip.empty()
                && message.candidate.port != 0
                && !message.candidate.type.empty()) {
                root["candidate"] = std::string("candidate:")
                                    + message.candidate.foundation + " "
                                    + std::to_string(message.candidate.component) + " "
                                    + message.candidate.transport + " "
                                    + std::to_string(message.candidate.priority) + " "
                                    + message.candidate.ip + " "
                                    + std::to_string(message.candidate.port) + " typ "
                                    + message.candidate.type;
            } else {
                root["candidate"] = message.candidate.candidate;
            }
            root["mid"] = message.candidate.mid;
            root["mline_index"] = message.candidate.mline_index;
            if (!message.candidate.foundation.empty()) {
                root["foundation"] = message.candidate.foundation;
            }
            if (message.candidate.component != 0) {
                root["component"] = message.candidate.component;
            }
            if (!message.candidate.transport.empty()) {
                root["transport"] = message.candidate.transport;
            }
            if (message.candidate.priority != 0) {
                root["priority"] = message.candidate.priority;
            }
            if (!message.candidate.ip.empty()) {
                root["ip"] = message.candidate.ip;
            }
            if (message.candidate.port != 0) {
                root["port"] = message.candidate.port;
            }
            if (!message.candidate.type.empty()) {
                root["candidate_type"] = message.candidate.type;
            }
            if (!message.candidate.related_address.empty()) {
                root["related_address"] = message.candidate.related_address;
            }
            if (message.candidate.related_port != 0) {
                root["related_port"] = message.candidate.related_port;
            }
            break;
        case SignalingMessage::Kind::ice_state:
            root["state"] = to_ice_state_text(message.ice_transport_state);
            break;
        case SignalingMessage::Kind::ice_nomination:
            root["state"] = to_ice_nomination_state_text(message.ice_nomination_state);
            root["selected_pair_reason"] = to_ice_selected_pair_reason_text(message.selected_pair_reason);
            root["selected_pair_reason_text"] = message.selected_pair_reason_text.empty()
                                                    ? to_ice_selected_pair_reason_text(message.selected_pair_reason)
                                                    : message.selected_pair_reason_text;
            if (!message.nomination_transaction_id.empty()) {
                root["nomination_transaction_id"] = message.nomination_transaction_id;
            }
            break;
        case SignalingMessage::Kind::diagnostics:
            {
                const bool release_mode_strict_v2 = diagnostics_emission_config_.release_mode_strict_v2;
                const bool emit_flat_compat_fields = release_mode_strict_v2
                                                       ? false
                                                       : diagnostics_emission_config_.emit_flat_compat_fields;
                root["scope"] = message.diagnostics_scope.empty() ? "peer_session" : message.diagnostics_scope;
                root["schema_version"] = emit_flat_compat_fields
                                             ? (message.diagnostics_schema_version.empty()
                                                    ? "v2"
                                                    : message.diagnostics_schema_version)
                                             : "v2";
                root["emit_flat_compat_fields"] = emit_flat_compat_fields;
                root["release_mode_strict_v2"] = release_mode_strict_v2;
                if (emit_flat_compat_fields) {
                    root["rollout_alert_window_seconds"] = message.diagnostics_rollout_alert_window_seconds;
                    root["rollout_mismatch_count_alert_threshold"] =
                        message.diagnostics_rollout_mismatch_count_alert_threshold;
                    root["rollout_mismatch_ratio_threshold_ppm"] =
                        message.diagnostics_rollout_mismatch_ratio_threshold_ppm;
                    root["rollout_current_mismatch_ratio_ppm"] =
                        message.diagnostics_rollout_current_mismatch_ratio_ppm;
                    root["rollout_alert_active"] =
                        message.diagnostics_rollout_alert_active;
                    root["rollout_progress_blocked"] =
                        message.diagnostics_rollout_progress_blocked;
                    root["rollout_ready_for_progress"] =
                        message.diagnostics_rollout_ready_for_progress;
                }
                {
                    Json rollout;
                    rollout["alert_window_seconds"] = message.diagnostics_rollout_alert_window_seconds;
                    rollout["mismatch_count_alert_threshold"] =
                        message.diagnostics_rollout_mismatch_count_alert_threshold;
                    rollout["mismatch_ratio_threshold_ppm"] =
                        message.diagnostics_rollout_mismatch_ratio_threshold_ppm;
                    rollout["current_mismatch_ratio_ppm"] =
                        message.diagnostics_rollout_current_mismatch_ratio_ppm;
                    rollout["alert_active"] = message.diagnostics_rollout_alert_active;
                    rollout["progress_blocked"] = message.diagnostics_rollout_progress_blocked;
                    rollout["ready_for_progress"] = message.diagnostics_rollout_ready_for_progress;
                    root["rollout_policy"] = std::move(rollout);
                }
                if (emit_flat_compat_fields) {
                    root["ice_nomination_state"] = to_ice_nomination_state_text(message.diagnostics_ice_nomination_state);
                    root["has_selected_pair"] = message.diagnostics_has_selected_pair;
                    root["selected_pair_reason"] = to_ice_selected_pair_reason_text(message.diagnostics_selected_pair_reason);
                    root["selected_pair_reason_text"] = message.diagnostics_selected_pair_reason_text;
                }
                {
                    Json nomination;
                    nomination["state"] = to_ice_nomination_state_text(message.diagnostics_ice_nomination_state);
                    nomination["has_selected_pair"] = message.diagnostics_has_selected_pair;
                    nomination["selected_pair_reason"] =
                        to_ice_selected_pair_reason_text(message.diagnostics_selected_pair_reason);
                    nomination["selected_pair_reason_text"] = message.diagnostics_selected_pair_reason_text;
                    root["nomination"] = std::move(nomination);
                }
                if (emit_flat_compat_fields) {
                    root["last_ice_error"] = message.diagnostics_last_ice_error;
                    root["stun_transaction_count"] = message.diagnostics_stun_transaction_count;
                    root["has_last_stun_transaction"] = message.diagnostics_has_last_stun_transaction;
                    root["last_stun_transaction_id"] = message.diagnostics_last_stun_transaction_id;
                    root["last_stun_transaction_state"] =
                        to_stun_transaction_state_text(message.diagnostics_last_stun_transaction_state);
                }
                {
                    Json stun;
                    stun["last_ice_error"] = message.diagnostics_last_ice_error;
                    stun["transaction_count"] = message.diagnostics_stun_transaction_count;
                    stun["has_last_transaction"] = message.diagnostics_has_last_stun_transaction;
                    stun["last_transaction_id"] = message.diagnostics_last_stun_transaction_id;
                    stun["last_transaction_state"] =
                        to_stun_transaction_state_text(message.diagnostics_last_stun_transaction_state);
                    root["stun"] = std::move(stun);
                }
                if (emit_flat_compat_fields) {
                    root["pending_nomination_signal_count"] = message.diagnostics_pending_nomination_signal_count;
                    root["dropped_nomination_signal_count"] = message.diagnostics_dropped_nomination_signal_count;
                    root["dropped_nomination_signal_overflow_count"] =
                        message.diagnostics_dropped_nomination_signal_overflow_count;
                    root["dropped_nomination_signal_trim_count"] =
                        message.diagnostics_dropped_nomination_signal_trim_count;
                    root["pending_diagnostics_signal_count"] =
                        message.diagnostics_pending_diagnostics_signal_count;
                    root["dropped_diagnostics_signal_count"] =
                        message.diagnostics_dropped_diagnostics_signal_count;
                    root["dropped_diagnostics_signal_overflow_count"] =
                        message.diagnostics_dropped_diagnostics_signal_overflow_count;
                    root["dropped_diagnostics_signal_trim_count"] =
                        message.diagnostics_dropped_diagnostics_signal_trim_count;
                    root["v2_flat_duplicate_seen_count"] =
                        message.diagnostics_v2_flat_duplicate_seen_count;
                    root["v2_flat_duplicate_mismatch_count"] =
                        message.diagnostics_v2_flat_duplicate_mismatch_count;
                }
                {
                    Json migration;
                    migration["v2_flat_duplicate_seen_count"] =
                        message.diagnostics_v2_flat_duplicate_seen_count;
                    migration["v2_flat_duplicate_mismatch_count"] =
                        message.diagnostics_v2_flat_duplicate_mismatch_count;
                    root["migration"] = std::move(migration);
                }
                {
                    Json queues;
                    Json nomination;
                    nomination["pending_signal_count"] = message.diagnostics_pending_nomination_signal_count;
                    nomination["dropped_signal_count"] = message.diagnostics_dropped_nomination_signal_count;
                    nomination["dropped_signal_overflow_count"] =
                        message.diagnostics_dropped_nomination_signal_overflow_count;
                    nomination["dropped_signal_trim_count"] =
                        message.diagnostics_dropped_nomination_signal_trim_count;
                    queues["nomination"] = std::move(nomination);

                    Json diagnostics;
                    diagnostics["pending_signal_count"] = message.diagnostics_pending_diagnostics_signal_count;
                    diagnostics["dropped_signal_count"] = message.diagnostics_dropped_diagnostics_signal_count;
                    diagnostics["dropped_signal_overflow_count"] =
                        message.diagnostics_dropped_diagnostics_signal_overflow_count;
                    diagnostics["dropped_signal_trim_count"] =
                        message.diagnostics_dropped_diagnostics_signal_trim_count;
                    queues["diagnostics"] = std::move(diagnostics);

                    root["queues"] = std::move(queues);
                }
                if (emit_flat_compat_fields) {
                    root["policy_keep_latest_only"] = message.diagnostics_policy_keep_latest_only;
                    root["policy_max_pending_signals"] = message.diagnostics_policy_max_pending_signals;
                    root["policy_nomination_max_pending_signals"] =
                        message.diagnostics_policy_nomination_max_pending_signals;
                }
                {
                    Json policy;
                    policy["keep_latest_only"] = message.diagnostics_policy_keep_latest_only;
                    policy["max_pending_signals"] = message.diagnostics_policy_max_pending_signals;
                    policy["nomination_max_pending_signals"] =
                        message.diagnostics_policy_nomination_max_pending_signals;
                    root["policy"] = std::move(policy);
                }
            }
            break;
        case SignalingMessage::Kind::dtls_state:
            root["state"] = to_dtls_state_text(message.dtls_transport_state);
            break;
        case SignalingMessage::Kind::security_state:
            {
                SecurityErrorCode code = message.security_error_code;
                if (code == SecurityErrorCode::none && !message.security_error.empty()) {
                    code = infer_security_error_code(message.security_error);
                }

                root["security_code"] = to_security_error_text(code);
                if (code == SecurityErrorCode::external_security_error) {
                    root["security_error"] = message.security_error.empty()
                                                 ? to_security_error_text(code)
                                                 : message.security_error;
                } else {
                    root["security_error"] = to_security_error_text(code);
                }
            }
            break;
        case SignalingMessage::Kind::unknown:
        default:
            break;
    }

    return root.dump();
}

bool WebrtcSignalingBridge::has_local_description() const
{
    return has_local_description_;
}

bool WebrtcSignalingBridge::has_remote_description() const
{
    return has_remote_description_;
}

bool WebrtcSignalingBridge::has_parsed_local_sdp() const
{
    return has_parsed_local_sdp_;
}

bool WebrtcSignalingBridge::has_parsed_remote_sdp() const
{
    return has_parsed_remote_sdp_;
}

SignalingState WebrtcSignalingBridge::signaling_state() const
{
    return signaling_state_;
}

const SessionDescription &WebrtcSignalingBridge::local_description() const
{
    return local_description_;
}

const SessionDescription &WebrtcSignalingBridge::remote_description() const
{
    return remote_description_;
}

const SdpSession &WebrtcSignalingBridge::parsed_local_sdp() const
{
    return parsed_local_sdp_;
}

const SdpSession &WebrtcSignalingBridge::parsed_remote_sdp() const
{
    return parsed_remote_sdp_;
}

const std::string &WebrtcSignalingBridge::last_sdp_error() const
{
    return last_sdp_error_;
}

const std::vector<IceCandidate> &WebrtcSignalingBridge::remote_candidates() const
{
    return remote_candidates_;
}

uint64_t WebrtcSignalingBridge::diagnostics_v2_flat_duplicate_seen_count() const
{
    return diagnostics_v2_flat_duplicate_seen_count_;
}

uint64_t WebrtcSignalingBridge::diagnostics_v2_flat_duplicate_mismatch_count() const
{
    return diagnostics_v2_flat_duplicate_mismatch_count_;
}

void WebrtcSignalingBridge::set_diagnostics_emission_config(const DiagnosticsEmissionConfig &config)
{
    diagnostics_emission_config_ = config;
    if (diagnostics_emission_config_.rollout_alert_window_seconds == 0) {
        diagnostics_emission_config_.rollout_alert_window_seconds = 1;
    }
}

WebrtcSignalingBridge::DiagnosticsEmissionConfig WebrtcSignalingBridge::diagnostics_emission_config() const
{
    return diagnostics_emission_config_;
}

bool WebrtcSignalingBridge::is_valid_local_transition(SdpType type) const
{
    switch (type) {
        case SdpType::offer:
            return signaling_state_ == SignalingState::new_ || signaling_state_ == SignalingState::stable;
        case SdpType::answer:
        case SdpType::pranswer:
            return signaling_state_ == SignalingState::have_remote_offer;
        case SdpType::rollback:
            return signaling_state_ == SignalingState::have_local_offer || signaling_state_ == SignalingState::have_remote_offer;
        default:
            return false;
    }
}

bool WebrtcSignalingBridge::is_valid_remote_transition(SdpType type) const
{
    switch (type) {
        case SdpType::offer:
            return signaling_state_ == SignalingState::new_ || signaling_state_ == SignalingState::stable;
        case SdpType::answer:
        case SdpType::pranswer:
            return signaling_state_ == SignalingState::have_local_offer;
        case SdpType::rollback:
            return signaling_state_ == SignalingState::have_local_offer || signaling_state_ == SignalingState::have_remote_offer;
        default:
            return false;
    }
}

bool WebrtcSignalingBridge::validate_local_negotiation(
    SdpType type,
    const SdpSession &parsed_sdp,
    std::string &out_error) const
{
    if (!has_parsed_remote_sdp_) {
        out_error.clear();
        return true;
    }

    if (type != SdpType::answer && type != SdpType::pranswer) {
        out_error.clear();
        return true;
    }

    return validate_answer_against_offer(parsed_remote_sdp_,
                                         parsed_sdp,
                                         kErrSdpMediaCountMismatch,
                                         kErrSdpMediaKindMismatch,
                                         kErrSdpMidMismatch,
                                         kErrSdpPayloadMismatch,
                                         out_error);
}

bool WebrtcSignalingBridge::validate_remote_negotiation(
    SdpType type,
    const SdpSession &parsed_sdp,
    std::string &out_error) const
{
    if (!has_parsed_local_sdp_) {
        out_error.clear();
        return true;
    }

    if (type != SdpType::answer && type != SdpType::pranswer) {
        out_error.clear();
        return true;
    }

    return validate_answer_against_offer(parsed_local_sdp_,
                                         parsed_sdp,
                                         kErrSdpMediaCountMismatch,
                                         kErrSdpMediaKindMismatch,
                                         kErrSdpMidMismatch,
                                         kErrSdpPayloadMismatch,
                                         out_error);
}

} // namespace yuan::net::webrtc
