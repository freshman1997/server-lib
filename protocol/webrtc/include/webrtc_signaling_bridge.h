#ifndef __NET_WEBRTC_SIGNALING_BRIDGE_H__
#define __NET_WEBRTC_SIGNALING_BRIDGE_H__

#include "webrtc_sdp.h"
#include "webrtc_types.h"

#include <string>
#include <vector>

namespace yuan::net::webrtc
{

class WebrtcSignalingBridge
{
public:
    struct DiagnosticsEmissionConfig
    {
        bool emit_flat_compat_fields = false;
        bool release_mode_strict_v2 = false;
        uint64_t rollout_alert_window_seconds = 86400;
        uint64_t rollout_mismatch_count_alert_threshold = 0;
        uint64_t rollout_mismatch_ratio_threshold_ppm = 1000;
    };

    struct SignalingMessage
    {
        enum class Kind
        {
            unknown,
            offer,
            answer,
            candidate,
            rollback,
            ice_state,
            ice_nomination,
            diagnostics,
            dtls_state,
            security_state,
        };

        Kind kind = Kind::unknown;
        SessionDescription description;
        IceCandidate candidate;
        IceTransportState ice_transport_state = IceTransportState::new_;
        IceNominationState ice_nomination_state = IceNominationState::none;
        IceSelectedPairReason selected_pair_reason = IceSelectedPairReason::none;
        std::string selected_pair_reason_text;
        std::string nomination_transaction_id;
        std::string diagnostics_scope;
        std::string diagnostics_schema_version = "v2";
        IceNominationState diagnostics_ice_nomination_state = IceNominationState::none;
        bool diagnostics_has_selected_pair = false;
        IceSelectedPairReason diagnostics_selected_pair_reason = IceSelectedPairReason::none;
        std::string diagnostics_selected_pair_reason_text;
        std::string diagnostics_last_ice_error;
        uint64_t diagnostics_stun_transaction_count = 0;
        bool diagnostics_has_last_stun_transaction = false;
        std::string diagnostics_last_stun_transaction_id;
        StunTransactionState diagnostics_last_stun_transaction_state = StunTransactionState::new_;
        uint64_t diagnostics_pending_nomination_signal_count = 0;
        uint64_t diagnostics_dropped_nomination_signal_count = 0;
        uint64_t diagnostics_dropped_nomination_signal_overflow_count = 0;
        uint64_t diagnostics_dropped_nomination_signal_trim_count = 0;
        uint64_t diagnostics_pending_diagnostics_signal_count = 0;
        uint64_t diagnostics_dropped_diagnostics_signal_count = 0;
        uint64_t diagnostics_dropped_diagnostics_signal_overflow_count = 0;
        uint64_t diagnostics_dropped_diagnostics_signal_trim_count = 0;
        uint64_t diagnostics_v2_flat_duplicate_seen_count = 0;
        uint64_t diagnostics_v2_flat_duplicate_mismatch_count = 0;
        uint64_t diagnostics_rollout_alert_window_seconds = 86400;
        uint64_t diagnostics_rollout_mismatch_count_alert_threshold = 0;
        uint64_t diagnostics_rollout_mismatch_ratio_threshold_ppm = 1000;
        uint64_t diagnostics_rollout_current_mismatch_ratio_ppm = 0;
        bool diagnostics_rollout_alert_active = false;
        bool diagnostics_rollout_progress_blocked = false;
        bool diagnostics_rollout_ready_for_progress = true;
        bool diagnostics_emit_flat_compat_fields = false;
        bool diagnostics_release_mode_strict_v2 = false;
        bool diagnostics_policy_keep_latest_only = true;
        uint64_t diagnostics_policy_max_pending_signals = 0;
        uint64_t diagnostics_policy_nomination_max_pending_signals = 0;
        DtlsTransportState dtls_transport_state = DtlsTransportState::new_;
        SecurityErrorCode security_error_code = SecurityErrorCode::none;
        std::string security_error;
    };

    bool set_local_description(const SessionDescription &description);
    bool set_remote_description(const SessionDescription &description);
    bool add_remote_candidate(const IceCandidate &candidate);

    bool apply_signaling_message(const SignalingMessage &message, bool as_remote);
    bool parse_signaling_message(const std::string &json_text, SignalingMessage &out_message) const;
    std::string to_signaling_json(const SignalingMessage &message) const;

    bool has_local_description() const;
    bool has_remote_description() const;
    bool has_parsed_local_sdp() const;
    bool has_parsed_remote_sdp() const;
    SignalingState signaling_state() const;

    const SessionDescription &local_description() const;
    const SessionDescription &remote_description() const;
    const SdpSession &parsed_local_sdp() const;
    const SdpSession &parsed_remote_sdp() const;
    const std::string &last_sdp_error() const;
    const std::vector<IceCandidate> &remote_candidates() const;
    void set_diagnostics_emission_config(const DiagnosticsEmissionConfig &config);
    DiagnosticsEmissionConfig diagnostics_emission_config() const;
    uint64_t diagnostics_v2_flat_duplicate_seen_count() const;
    uint64_t diagnostics_v2_flat_duplicate_mismatch_count() const;

private:
    static constexpr const char *kErrEmptySdp = "empty_sdp";
    static constexpr const char *kErrInvalidStateTransition = "invalid_state_transition";
    static constexpr const char *kErrSdpParseFailed = "sdp_parse_failed";
    static constexpr const char *kErrSdpMediaCountMismatch = "sdp_media_count_mismatch";
    static constexpr const char *kErrSdpMediaKindMismatch = "sdp_media_kind_mismatch";
    static constexpr const char *kErrSdpMidMismatch = "sdp_mid_mismatch";
    static constexpr const char *kErrSdpPayloadMismatch = "sdp_payload_mismatch";

    bool is_valid_local_transition(SdpType type) const;
    bool is_valid_remote_transition(SdpType type) const;
    bool validate_local_negotiation(SdpType type, const SdpSession &parsed_sdp, std::string &out_error) const;
    bool validate_remote_negotiation(SdpType type, const SdpSession &parsed_sdp, std::string &out_error) const;

    bool has_local_description_ = false;
    bool has_remote_description_ = false;
    bool has_parsed_local_sdp_ = false;
    bool has_parsed_remote_sdp_ = false;
    SignalingState signaling_state_ = SignalingState::new_;
    SessionDescription local_description_;
    SessionDescription remote_description_;
    SdpSession parsed_local_sdp_;
    SdpSession parsed_remote_sdp_;
    std::string last_sdp_error_;
    std::vector<IceCandidate> remote_candidates_;
    DiagnosticsEmissionConfig diagnostics_emission_config_;
    mutable uint64_t diagnostics_v2_flat_duplicate_seen_count_ = 0;
    mutable uint64_t diagnostics_v2_flat_duplicate_mismatch_count_ = 0;
};

} // namespace yuan::net::webrtc

#endif
