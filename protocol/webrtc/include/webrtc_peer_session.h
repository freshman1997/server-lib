#ifndef __NET_WEBRTC_PEER_SESSION_H__
#define __NET_WEBRTC_PEER_SESSION_H__

#include "webrtc_dtls_transport.h"
#include "webrtc_ice_transport.h"
#include "webrtc_signaling_bridge.h"
#include "webrtc_transport_bridge.h"

#include <cstddef>
#include <functional>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::webrtc
{

struct WebrtcPeerSessionSnapshot
{
    SignalingState signaling_state = SignalingState::new_;
    IceTransportState ice_transport_state = IceTransportState::new_;
    DtlsTransportState dtls_transport_state = DtlsTransportState::new_;
    PeerConnectionState connection_state = PeerConnectionState::new_;
    bool has_local_description = false;
    bool has_remote_description = false;
    std::size_t remote_candidate_count = 0;
    IceGatheringState ice_gathering_state = IceGatheringState::new_;
    IceChecklistState ice_checklist_state = IceChecklistState::idle;
    IceNominationState ice_nomination_state = IceNominationState::none;
    bool has_selected_ice_pair = false;
    IceSelectedPairReason selected_ice_pair_reason = IceSelectedPairReason::none;
    std::string selected_ice_pair_reason_text;
    std::string selected_ice_pair_nomination_transaction_id;
    std::string last_ice_error;
    std::vector<StunTransaction> stun_transactions;
    bool has_last_stun_transaction = false;
    StunTransaction last_stun_transaction;
    std::size_t pending_ice_nomination_signal_count = 0;
    std::size_t peak_pending_ice_nomination_signal_count = 0;
    uint64_t dropped_ice_nomination_signal_count = 0;
    uint64_t dropped_ice_nomination_signal_overflow_count = 0;
    uint64_t dropped_ice_nomination_signal_trim_count = 0;
    std::size_t pending_diagnostics_signal_count = 0;
    std::size_t peak_pending_diagnostics_signal_count = 0;
    uint64_t dropped_diagnostics_signal_count = 0;
    uint64_t dropped_diagnostics_signal_overflow_count = 0;
    uint64_t dropped_diagnostics_signal_trim_count = 0;
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
    std::size_t diagnostics_policy_max_pending_signals = 8;
    std::size_t diagnostics_policy_nomination_max_pending_signals = 16;
    bool transport_ready = false;
    bool media_ready = false;
    bool scheduler_active = false;
    bool dtls_fingerprint_consistent = false;
    bool dtls_fingerprint_policy_required = false;
    std::string security_error;
    SecurityErrorCode security_error_code = SecurityErrorCode::none;
};

struct NominationSignalQueueConfig
{
    std::size_t max_pending_signals = 16;
};

struct DiagnosticsSignalQueueConfig
{
    bool keep_latest_only = true;
    std::size_t max_pending_signals = 8;
};

struct SignalQueueRuntimeConfig
{
    std::size_t nomination_max_pending_signals = 16;
    bool diagnostics_keep_latest_only = true;
    std::size_t diagnostics_max_pending_signals = 8;
    bool diagnostics_emit_flat_compat_fields = false;
    bool diagnostics_release_mode_strict_v2 = false;
    uint64_t diagnostics_rollout_alert_window_seconds = 86400;
    uint64_t diagnostics_rollout_mismatch_count_alert_threshold = 0;
    uint64_t diagnostics_rollout_mismatch_ratio_threshold_ppm = 1000;
};

class WebrtcPeerSession
{
public:
    using ConnectionStateCallback = std::function<void(PeerConnectionState previous_state, PeerConnectionState current_state)>;

    explicit WebrtcPeerSession(uint32_t local_ssrc = 0, uint32_t clock_rate = 90000);

    bool apply_signaling_message(const WebrtcSignalingBridge::SignalingMessage &message, bool as_remote, uint64_t now_ms = 0);
    bool apply_signaling_json(const std::string &json_text, bool as_remote, uint64_t now_ms = 0);

    bool on_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms);
    void set_srtp_context(std::shared_ptr<SrtpContext> context);
    bool has_srtp_context() const;
    void on_sender_activity(uint64_t ntp_timestamp, uint32_t rtp_timestamp, uint32_t packet_count, uint32_t octet_count);

    void set_rtcp_schedule_config(const RtcpScheduleConfig &config);
    RtcpScheduleConfig rtcp_schedule_config() const;
    bool poll_scheduled_rtcp(uint64_t now_ms, ::yuan::net::rtcp::RtcpPacket &out_packet);

    void set_ice_transport_state(IceTransportState state);
    IceTransportState ice_transport_state() const;
    IceGatheringState ice_gathering_state() const;
    IceChecklistState ice_checklist_state() const;
    IceNominationState ice_nomination_state() const;
    void start_ice_gathering(uint64_t now_ms = 0);
    void set_ice_transport_engine(std::shared_ptr<IceTransportEngine> engine);
    void set_ice_provider(std::shared_ptr<IceTransportProvider> provider);
    void set_local_ice_candidates(const std::vector<IceCandidate> &candidates);
    std::vector<IceCandidate> local_ice_candidates() const;
    std::vector<IceCandidate> remote_ice_candidates() const;
    bool has_selected_ice_pair() const;
    IceCandidatePair selected_ice_pair() const;
    const std::string &last_ice_error() const;
    void set_mock_ice_transport_config(const MockIceTransportConfig &config);
    MockIceTransportConfig mock_ice_transport_config() const;
    void set_dtls_transport_state(DtlsTransportState state);
    DtlsTransportState dtls_transport_state() const;
    void set_dtls_transport_engine(std::shared_ptr<DtlsTransportEngine> engine);
    void set_mock_dtls_transport_config(const MockDtlsTransportConfig &config);
    MockDtlsTransportConfig mock_dtls_transport_config() const;
    void advance_transport(uint64_t now_ms);
    bool is_transport_ready() const;
    bool has_dtls_srtp_keying_material() const;
    DtlsSrtpKeyingMaterial dtls_srtp_keying_material() const;
    DtlsFingerprintVerificationState dtls_fingerprint_verification_state() const;
    DtlsPeerFingerprint dtls_peer_fingerprint() const;
    bool is_dtls_fingerprint_consistent_with_remote_sdp() const;
    void set_require_dtls_fingerprint_match_for_media(bool required);
    bool require_dtls_fingerprint_match_for_media() const;
    const std::string &last_security_error() const;
    SecurityErrorCode last_security_error_code() const;

    bool is_media_ready() const;
    PeerConnectionState connection_state() const;
    void set_connection_state_callback(ConnectionStateCallback callback);
    WebrtcPeerSessionSnapshot snapshot() const;
    std::string snapshot_json() const;
    bool parse_snapshot_json(const std::string &json_text, WebrtcPeerSessionSnapshot &out_snapshot) const;
    std::string signal_queue_runtime_config_json() const;
    bool parse_signal_queue_runtime_config_json(const std::string &json_text, SignalQueueRuntimeConfig &out_config) const;
    std::string rollout_health_json() const;
    SignalingState signaling_state() const;
    bool poll_ice_nomination_signal(WebrtcSignalingBridge::SignalingMessage &out_message);
    bool poll_diagnostics_signal(WebrtcSignalingBridge::SignalingMessage &out_message);
    void set_nomination_signal_queue_config(const NominationSignalQueueConfig &config);
    NominationSignalQueueConfig nomination_signal_queue_config() const;
    void set_diagnostics_signal_queue_config(const DiagnosticsSignalQueueConfig &config);
    DiagnosticsSignalQueueConfig diagnostics_signal_queue_config() const;
    void set_signal_queue_runtime_config(const SignalQueueRuntimeConfig &config);
    SignalQueueRuntimeConfig signal_queue_runtime_config() const;

    WebrtcSignalingBridge &signaling_bridge()
    {
        return signaling_bridge_;
    }

    const WebrtcSignalingBridge &signaling_bridge() const
    {
        return signaling_bridge_;
    }

    WebrtcTransportBridge &transport_bridge()
    {
        return transport_bridge_;
    }

    const WebrtcTransportBridge &transport_bridge() const
    {
        return transport_bridge_;
    }

private:
    void refresh_scheduler_activation(uint64_t now_ms);
    void refresh_connection_state();
    void update_connection_state(PeerConnectionState next_state);
    void sync_transport_from_mocks(uint64_t now_ms);
    void refresh_ice_nomination_signal();
    bool security_gate_passed(std::string *out_error = nullptr) const;

    WebrtcSignalingBridge signaling_bridge_;
    WebrtcTransportBridge transport_bridge_;
    std::shared_ptr<IceTransportEngine> ice_transport_engine_;
    std::shared_ptr<DtlsTransportEngine> dtls_transport_engine_;
    IceTransportState ice_transport_state_ = IceTransportState::new_;
    DtlsTransportState dtls_transport_state_ = DtlsTransportState::new_;
    PeerConnectionState connection_state_ = PeerConnectionState::new_;
    ConnectionStateCallback connection_state_callback_;
    bool scheduler_active_ = false;
    bool require_dtls_fingerprint_match_for_media_ = false;
    std::string last_ice_error_;
    std::string last_security_error_;
    SecurityErrorCode last_security_error_code_ = SecurityErrorCode::none;
    std::string injected_security_error_;
    IceNominationState last_notified_nomination_state_ = IceNominationState::none;
    std::string last_notified_nomination_transaction_id_;
    std::deque<WebrtcSignalingBridge::SignalingMessage> pending_ice_nomination_signals_;
    std::deque<WebrtcSignalingBridge::SignalingMessage> pending_diagnostics_signals_;
    NominationSignalQueueConfig nomination_signal_queue_config_;
    DiagnosticsSignalQueueConfig diagnostics_signal_queue_config_;
    std::size_t peak_pending_ice_nomination_signal_count_ = 0;
    uint64_t dropped_ice_nomination_signal_count_ = 0;
    uint64_t dropped_ice_nomination_signal_overflow_count_ = 0;
    uint64_t dropped_ice_nomination_signal_trim_count_ = 0;
    std::size_t peak_pending_diagnostics_signal_count_ = 0;
    uint64_t dropped_diagnostics_signal_count_ = 0;
    uint64_t dropped_diagnostics_signal_overflow_count_ = 0;
    uint64_t dropped_diagnostics_signal_trim_count_ = 0;
};

} // namespace yuan::net::webrtc

#endif
