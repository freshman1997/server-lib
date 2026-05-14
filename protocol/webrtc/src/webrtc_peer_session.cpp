#include "webrtc_peer_session.h"
#include "webrtc_security_utils.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <utility>

namespace
{

using Json = nlohmann::json;

const char *to_signaling_state_text(::yuan::net::webrtc::SignalingState state)
{
    switch (state) {
        case ::yuan::net::webrtc::SignalingState::new_:
            return "new";
        case ::yuan::net::webrtc::SignalingState::have_local_offer:
            return "have_local_offer";
        case ::yuan::net::webrtc::SignalingState::have_remote_offer:
            return "have_remote_offer";
        case ::yuan::net::webrtc::SignalingState::stable:
            return "stable";
        default:
            return "new";
    }
}

bool parse_signaling_state_text(const std::string &text, ::yuan::net::webrtc::SignalingState &out)
{
    if (text == "new") {
        out = ::yuan::net::webrtc::SignalingState::new_;
        return true;
    }
    if (text == "have_local_offer") {
        out = ::yuan::net::webrtc::SignalingState::have_local_offer;
        return true;
    }
    if (text == "have_remote_offer") {
        out = ::yuan::net::webrtc::SignalingState::have_remote_offer;
        return true;
    }
    if (text == "stable") {
        out = ::yuan::net::webrtc::SignalingState::stable;
        return true;
    }
    return false;
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

bool parse_ice_state_text(const std::string &text, ::yuan::net::webrtc::IceTransportState &out)
{
    if (text == "new") {
        out = ::yuan::net::webrtc::IceTransportState::new_;
        return true;
    }
    if (text == "checking") {
        out = ::yuan::net::webrtc::IceTransportState::checking;
        return true;
    }
    if (text == "connected") {
        out = ::yuan::net::webrtc::IceTransportState::connected;
        return true;
    }
    if (text == "failed") {
        out = ::yuan::net::webrtc::IceTransportState::failed;
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

bool parse_dtls_state_text(const std::string &text, ::yuan::net::webrtc::DtlsTransportState &out)
{
    if (text == "new") {
        out = ::yuan::net::webrtc::DtlsTransportState::new_;
        return true;
    }
    if (text == "connecting") {
        out = ::yuan::net::webrtc::DtlsTransportState::connecting;
        return true;
    }
    if (text == "connected") {
        out = ::yuan::net::webrtc::DtlsTransportState::connected;
        return true;
    }
    if (text == "failed") {
        out = ::yuan::net::webrtc::DtlsTransportState::failed;
        return true;
    }
    return false;
}

const char *to_connection_state_text(::yuan::net::webrtc::PeerConnectionState state)
{
    switch (state) {
        case ::yuan::net::webrtc::PeerConnectionState::new_:
            return "new";
        case ::yuan::net::webrtc::PeerConnectionState::connecting:
            return "connecting";
        case ::yuan::net::webrtc::PeerConnectionState::connected:
            return "connected";
        case ::yuan::net::webrtc::PeerConnectionState::failed:
            return "failed";
        default:
            return "new";
    }
}

bool parse_connection_state_text(const std::string &text, ::yuan::net::webrtc::PeerConnectionState &out)
{
    if (text == "new") {
        out = ::yuan::net::webrtc::PeerConnectionState::new_;
        return true;
    }
    if (text == "connecting") {
        out = ::yuan::net::webrtc::PeerConnectionState::connecting;
        return true;
    }
    if (text == "connected") {
        out = ::yuan::net::webrtc::PeerConnectionState::connected;
        return true;
    }
    if (text == "failed") {
        out = ::yuan::net::webrtc::PeerConnectionState::failed;
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

} // namespace

namespace yuan::net::webrtc
{

WebrtcPeerSession::WebrtcPeerSession(uint32_t local_ssrc, uint32_t clock_rate)
    : transport_bridge_(local_ssrc, clock_rate),
      ice_transport_engine_(std::make_shared<MockIceTransport>()),
      dtls_transport_engine_(std::make_shared<MockDtlsTransport>())
{
    nomination_signal_queue_config_.max_pending_signals = 16;
    diagnostics_signal_queue_config_.keep_latest_only = true;
    diagnostics_signal_queue_config_.max_pending_signals = 8;
    pending_ice_nomination_signals_.clear();
    pending_diagnostics_signals_.clear();
    peak_pending_ice_nomination_signal_count_ = 0;
    dropped_ice_nomination_signal_count_ = 0;
    dropped_ice_nomination_signal_overflow_count_ = 0;
    dropped_ice_nomination_signal_trim_count_ = 0;
    peak_pending_diagnostics_signal_count_ = 0;
    dropped_diagnostics_signal_count_ = 0;
    dropped_diagnostics_signal_overflow_count_ = 0;
    dropped_diagnostics_signal_trim_count_ = 0;
}

bool WebrtcPeerSession::apply_signaling_message(
    const WebrtcSignalingBridge::SignalingMessage &message,
    bool as_remote,
    uint64_t now_ms)
{
    if (message.kind == WebrtcSignalingBridge::SignalingMessage::Kind::ice_state) {
        ice_transport_engine_->set_state(message.ice_transport_state);
        sync_transport_from_mocks(now_ms);
        refresh_scheduler_activation(now_ms);
        refresh_connection_state();
        return true;
    }

    if (message.kind == WebrtcSignalingBridge::SignalingMessage::Kind::ice_nomination) {
        if (ice_transport_engine_) {
            ice_transport_engine_->set_nomination_from_signal(
                message.ice_nomination_state,
                message.selected_pair_reason,
                message.selected_pair_reason_text,
                message.nomination_transaction_id);
            ice_transport_state_ = ice_transport_engine_->state();
            last_ice_error_ = ice_transport_engine_->last_error();
        }
        refresh_ice_nomination_signal();
        refresh_scheduler_activation(now_ms);
        refresh_connection_state();
        return true;
    }

    if (message.kind == WebrtcSignalingBridge::SignalingMessage::Kind::dtls_state) {
        dtls_transport_engine_->set_state(message.dtls_transport_state);
        sync_transport_from_mocks(now_ms);
        refresh_scheduler_activation(now_ms);
        refresh_connection_state();
        return true;
    }

    if (message.kind == WebrtcSignalingBridge::SignalingMessage::Kind::security_state) {
        last_security_error_code_ = message.security_error_code;
        injected_security_error_ = message.security_error.empty()
                                     ? to_security_error_text(message.security_error_code)
                                     : message.security_error;
        last_security_error_ = injected_security_error_;
        refresh_scheduler_activation(now_ms);
        refresh_connection_state();
        return true;
    }

    if (!signaling_bridge_.apply_signaling_message(message, as_remote)) {
        return false;
    }

    if (message.kind == WebrtcSignalingBridge::SignalingMessage::Kind::candidate && as_remote) {
        ice_transport_engine_->on_remote_candidate(message.candidate, now_ms);
    }

    sync_transport_from_mocks(now_ms);
    refresh_scheduler_activation(now_ms);
    refresh_connection_state();
    return true;
}

bool WebrtcPeerSession::apply_signaling_json(const std::string &json_text, bool as_remote, uint64_t now_ms)
{
    WebrtcSignalingBridge::SignalingMessage message;
    if (!signaling_bridge_.parse_signaling_message(json_text, message)) {
        return false;
    }
    return apply_signaling_message(message, as_remote, now_ms);
}

bool WebrtcPeerSession::on_rtp_packet_received(const ::yuan::net::rtc::RtcPacket &packet, uint64_t arrival_time_ms)
{
    return transport_bridge_.on_srtp_rtp_packet_received(packet, arrival_time_ms);
}

void WebrtcPeerSession::set_srtp_context(std::shared_ptr<SrtpContext> context)
{
    transport_bridge_.set_srtp_context(std::move(context));
    if (transport_bridge_.has_srtp_context() && dtls_transport_engine_ && dtls_transport_engine_->has_srtp_keying_material()) {
        std::shared_ptr<SrtpContext> ctx = transport_bridge_.srtp_context();
        if (ctx) {
            (void) ctx->apply_keying_material(dtls_transport_engine_->srtp_keying_material());
        }
    }
}

bool WebrtcPeerSession::has_srtp_context() const
{
    return transport_bridge_.has_srtp_context();
}

void WebrtcPeerSession::on_sender_activity(
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    uint32_t packet_count,
    uint32_t octet_count)
{
    transport_bridge_.on_sender_activity(ntp_timestamp, rtp_timestamp, packet_count, octet_count);
}

void WebrtcPeerSession::set_rtcp_schedule_config(const RtcpScheduleConfig &config)
{
    transport_bridge_.set_rtcp_schedule_config(config);
}

RtcpScheduleConfig WebrtcPeerSession::rtcp_schedule_config() const
{
    return transport_bridge_.rtcp_schedule_config();
}

bool WebrtcPeerSession::poll_scheduled_rtcp(uint64_t now_ms, ::yuan::net::rtcp::RtcpPacket &out_packet)
{
    if (!scheduler_active_) {
        return false;
    }
    return transport_bridge_.poll_scheduled_rtcp(now_ms, out_packet);
}

bool WebrtcPeerSession::poll_ice_nomination_signal(WebrtcSignalingBridge::SignalingMessage &out_message)
{
    if (pending_ice_nomination_signals_.empty()) {
        return false;
    }
    out_message = pending_ice_nomination_signals_.front();
    pending_ice_nomination_signals_.pop_front();
    return true;
}

bool WebrtcPeerSession::poll_diagnostics_signal(WebrtcSignalingBridge::SignalingMessage &out_message)
{
    if (pending_diagnostics_signals_.empty()) {
        return false;
    }
    out_message = pending_diagnostics_signals_.front();
    pending_diagnostics_signals_.pop_front();
    return true;
}

void WebrtcPeerSession::set_nomination_signal_queue_config(const NominationSignalQueueConfig &config)
{
    nomination_signal_queue_config_ = config;
    if (nomination_signal_queue_config_.max_pending_signals == 0) {
        nomination_signal_queue_config_.max_pending_signals = 1;
    }
    while (pending_ice_nomination_signals_.size() > nomination_signal_queue_config_.max_pending_signals) {
        pending_ice_nomination_signals_.pop_front();
        ++dropped_ice_nomination_signal_count_;
        ++dropped_ice_nomination_signal_trim_count_;
    }
}

NominationSignalQueueConfig WebrtcPeerSession::nomination_signal_queue_config() const
{
    return nomination_signal_queue_config_;
}

void WebrtcPeerSession::set_diagnostics_signal_queue_config(const DiagnosticsSignalQueueConfig &config)
{
    diagnostics_signal_queue_config_ = config;
    if (diagnostics_signal_queue_config_.max_pending_signals == 0) {
        diagnostics_signal_queue_config_.max_pending_signals = 1;
    }
    if (diagnostics_signal_queue_config_.keep_latest_only) {
        while (pending_diagnostics_signals_.size() > 1) {
            pending_diagnostics_signals_.pop_front();
            ++dropped_diagnostics_signal_count_;
            ++dropped_diagnostics_signal_trim_count_;
        }
    } else {
        while (pending_diagnostics_signals_.size() > diagnostics_signal_queue_config_.max_pending_signals) {
            pending_diagnostics_signals_.pop_front();
            ++dropped_diagnostics_signal_count_;
            ++dropped_diagnostics_signal_trim_count_;
        }
    }
}

DiagnosticsSignalQueueConfig WebrtcPeerSession::diagnostics_signal_queue_config() const
{
    return diagnostics_signal_queue_config_;
}

void WebrtcPeerSession::set_signal_queue_runtime_config(const SignalQueueRuntimeConfig &config)
{
    NominationSignalQueueConfig nomination;
    nomination.max_pending_signals = config.nomination_max_pending_signals;
    set_nomination_signal_queue_config(nomination);

    DiagnosticsSignalQueueConfig diagnostics;
    diagnostics.keep_latest_only = config.diagnostics_keep_latest_only;
    diagnostics.max_pending_signals = config.diagnostics_max_pending_signals;
    set_diagnostics_signal_queue_config(diagnostics);

    WebrtcSignalingBridge::DiagnosticsEmissionConfig emission;
    emission.emit_flat_compat_fields = config.diagnostics_emit_flat_compat_fields;
    emission.release_mode_strict_v2 = config.diagnostics_release_mode_strict_v2;
    emission.rollout_alert_window_seconds = config.diagnostics_rollout_alert_window_seconds;
    emission.rollout_mismatch_count_alert_threshold =
        config.diagnostics_rollout_mismatch_count_alert_threshold;
    emission.rollout_mismatch_ratio_threshold_ppm =
        config.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    signaling_bridge_.set_diagnostics_emission_config(emission);
}

SignalQueueRuntimeConfig WebrtcPeerSession::signal_queue_runtime_config() const
{
    SignalQueueRuntimeConfig out;
    out.nomination_max_pending_signals = nomination_signal_queue_config_.max_pending_signals;
    out.diagnostics_keep_latest_only = diagnostics_signal_queue_config_.keep_latest_only;
    out.diagnostics_max_pending_signals = diagnostics_signal_queue_config_.max_pending_signals;
    out.diagnostics_emit_flat_compat_fields =
        signaling_bridge_.diagnostics_emission_config().emit_flat_compat_fields;
    out.diagnostics_release_mode_strict_v2 =
        signaling_bridge_.diagnostics_emission_config().release_mode_strict_v2;
    out.diagnostics_rollout_alert_window_seconds =
        signaling_bridge_.diagnostics_emission_config().rollout_alert_window_seconds;
    out.diagnostics_rollout_mismatch_count_alert_threshold =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_count_alert_threshold;
    out.diagnostics_rollout_mismatch_ratio_threshold_ppm =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_ratio_threshold_ppm;
    return out;
}

std::string WebrtcPeerSession::signal_queue_runtime_config_json() const
{
    const SignalQueueRuntimeConfig cfg = signal_queue_runtime_config();
    Json root;
    root["nomination_max_pending_signals"] = cfg.nomination_max_pending_signals;
    root["diagnostics_keep_latest_only"] = cfg.diagnostics_keep_latest_only;
    root["diagnostics_max_pending_signals"] = cfg.diagnostics_max_pending_signals;
    root["diagnostics_emit_flat_compat_fields"] = cfg.diagnostics_emit_flat_compat_fields;
    root["diagnostics_release_mode_strict_v2"] = cfg.diagnostics_release_mode_strict_v2;
    root["diagnostics_rollout_alert_window_seconds"] = cfg.diagnostics_rollout_alert_window_seconds;
    root["diagnostics_rollout_mismatch_count_alert_threshold"] =
        cfg.diagnostics_rollout_mismatch_count_alert_threshold;
    root["diagnostics_rollout_mismatch_ratio_threshold_ppm"] =
        cfg.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    return root.dump();
}

bool WebrtcPeerSession::parse_signal_queue_runtime_config_json(
    const std::string &json_text,
    SignalQueueRuntimeConfig &out_config) const
{
    try {
        const Json root = Json::parse(json_text);
        if (!root.is_object()) {
            return false;
        }

        SignalQueueRuntimeConfig parsed;
        if (root.contains("nomination_max_pending_signals")) {
            if (!root["nomination_max_pending_signals"].is_number_unsigned()) {
                return false;
            }
            parsed.nomination_max_pending_signals =
                root["nomination_max_pending_signals"].get<std::size_t>();
        }
        if (root.contains("diagnostics_keep_latest_only")) {
            if (!root["diagnostics_keep_latest_only"].is_boolean()) {
                return false;
            }
            parsed.diagnostics_keep_latest_only = root["diagnostics_keep_latest_only"].get<bool>();
        }
        if (root.contains("diagnostics_max_pending_signals")) {
            if (!root["diagnostics_max_pending_signals"].is_number_unsigned()) {
                return false;
            }
            parsed.diagnostics_max_pending_signals =
                root["diagnostics_max_pending_signals"].get<std::size_t>();
        }
        if (root.contains("diagnostics_emit_flat_compat_fields")) {
            if (!root["diagnostics_emit_flat_compat_fields"].is_boolean()) {
                return false;
            }
            parsed.diagnostics_emit_flat_compat_fields =
                root["diagnostics_emit_flat_compat_fields"].get<bool>();
        }
        if (root.contains("diagnostics_release_mode_strict_v2")) {
            if (!root["diagnostics_release_mode_strict_v2"].is_boolean()) {
                return false;
            }
            parsed.diagnostics_release_mode_strict_v2 =
                root["diagnostics_release_mode_strict_v2"].get<bool>();
        }
        if (root.contains("diagnostics_rollout_alert_window_seconds")) {
            if (!root["diagnostics_rollout_alert_window_seconds"].is_number_unsigned()) {
                return false;
            }
            parsed.diagnostics_rollout_alert_window_seconds =
                root["diagnostics_rollout_alert_window_seconds"].get<uint64_t>();
        }
        if (root.contains("diagnostics_rollout_mismatch_count_alert_threshold")) {
            if (!root["diagnostics_rollout_mismatch_count_alert_threshold"].is_number_unsigned()) {
                return false;
            }
            parsed.diagnostics_rollout_mismatch_count_alert_threshold =
                root["diagnostics_rollout_mismatch_count_alert_threshold"].get<uint64_t>();
        }
        if (root.contains("diagnostics_rollout_mismatch_ratio_threshold_ppm")) {
            if (!root["diagnostics_rollout_mismatch_ratio_threshold_ppm"].is_number_unsigned()) {
                return false;
            }
            parsed.diagnostics_rollout_mismatch_ratio_threshold_ppm =
                root["diagnostics_rollout_mismatch_ratio_threshold_ppm"].get<uint64_t>();
        }

        if (parsed.nomination_max_pending_signals == 0) {
            parsed.nomination_max_pending_signals = 1;
        }
        if (parsed.diagnostics_max_pending_signals == 0) {
            parsed.diagnostics_max_pending_signals = 1;
        }
        if (parsed.diagnostics_rollout_alert_window_seconds == 0) {
            parsed.diagnostics_rollout_alert_window_seconds = 1;
        }

        out_config = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

std::string WebrtcPeerSession::rollout_health_json() const
{
    const WebrtcPeerSessionSnapshot s = snapshot();
    Json root;
    root["ready"] = s.diagnostics_rollout_ready_for_progress;
    root["rollout_ready_for_progress"] = s.diagnostics_rollout_ready_for_progress;
    root["rollout_progress_blocked"] = s.diagnostics_rollout_progress_blocked;
    root["rollout_alert_active"] = s.diagnostics_rollout_alert_active;
    root["rollout_current_mismatch_ratio_ppm"] = s.diagnostics_rollout_current_mismatch_ratio_ppm;
    root["rollout_alert_window_seconds"] = s.diagnostics_rollout_alert_window_seconds;
    root["rollout_mismatch_count_alert_threshold"] =
        s.diagnostics_rollout_mismatch_count_alert_threshold;
    root["rollout_mismatch_ratio_threshold_ppm"] =
        s.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    root["v2_flat_duplicate_seen_count"] = s.diagnostics_v2_flat_duplicate_seen_count;
    root["v2_flat_duplicate_mismatch_count"] = s.diagnostics_v2_flat_duplicate_mismatch_count;
    root["diagnostics_release_mode_strict_v2"] = s.diagnostics_release_mode_strict_v2;
    root["diagnostics_emit_flat_compat_fields"] = s.diagnostics_emit_flat_compat_fields;
    return root.dump();
}

void WebrtcPeerSession::set_ice_transport_state(IceTransportState state)
{
    ice_transport_engine_->set_state(state);
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

IceTransportState WebrtcPeerSession::ice_transport_state() const
{
    return ice_transport_state_;
}

IceGatheringState WebrtcPeerSession::ice_gathering_state() const
{
    if (!ice_transport_engine_) {
        return IceGatheringState::new_;
    }
    return ice_transport_engine_->gathering_state();
}

IceChecklistState WebrtcPeerSession::ice_checklist_state() const
{
    if (!ice_transport_engine_) {
        return IceChecklistState::idle;
    }
    return ice_transport_engine_->checklist_state();
}

IceNominationState WebrtcPeerSession::ice_nomination_state() const
{
    if (!ice_transport_engine_) {
        return IceNominationState::none;
    }
    return ice_transport_engine_->nomination_state();
}

void WebrtcPeerSession::start_ice_gathering(uint64_t now_ms)
{
    if (!ice_transport_engine_) {
        ice_transport_engine_ = std::make_shared<MockIceTransport>();
    }
    ice_transport_engine_->start_gathering(now_ms);
    sync_transport_from_mocks(now_ms);
    refresh_scheduler_activation(now_ms);
    refresh_connection_state();
}

void WebrtcPeerSession::set_ice_transport_engine(std::shared_ptr<IceTransportEngine> engine)
{
    ice_transport_engine_ = engine ? std::move(engine) : std::make_shared<MockIceTransport>();
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

void WebrtcPeerSession::set_ice_provider(std::shared_ptr<IceTransportProvider> provider)
{
    if (!ice_transport_engine_) {
        ice_transport_engine_ = std::make_shared<MockIceTransport>();
    }
    ice_transport_engine_->set_provider(std::move(provider));
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

void WebrtcPeerSession::set_local_ice_candidates(const std::vector<IceCandidate> &candidates)
{
    if (!ice_transport_engine_) {
        ice_transport_engine_ = std::make_shared<MockIceTransport>();
    }
    ice_transport_engine_->set_local_candidates(candidates);
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

std::vector<IceCandidate> WebrtcPeerSession::local_ice_candidates() const
{
    if (!ice_transport_engine_) {
        return {};
    }
    return ice_transport_engine_->local_candidates();
}

std::vector<IceCandidate> WebrtcPeerSession::remote_ice_candidates() const
{
    if (!ice_transport_engine_) {
        return {};
    }
    return ice_transport_engine_->remote_candidates();
}

bool WebrtcPeerSession::has_selected_ice_pair() const
{
    if (!ice_transport_engine_) {
        return false;
    }
    return ice_transport_engine_->has_selected_pair();
}

IceCandidatePair WebrtcPeerSession::selected_ice_pair() const
{
    if (!ice_transport_engine_) {
        return {};
    }
    return ice_transport_engine_->selected_pair();
}

const std::string &WebrtcPeerSession::last_ice_error() const
{
    return last_ice_error_;
}

void WebrtcPeerSession::set_mock_ice_transport_config(const MockIceTransportConfig &config)
{
    auto mock = std::dynamic_pointer_cast<MockIceTransport>(ice_transport_engine_);
    if (!mock) {
        mock = std::make_shared<MockIceTransport>();
        ice_transport_engine_ = mock;
    }
    mock->set_config(config);
}

MockIceTransportConfig WebrtcPeerSession::mock_ice_transport_config() const
{
    const auto mock = std::dynamic_pointer_cast<MockIceTransport>(ice_transport_engine_);
    if (!mock) {
        return {};
    }
    return mock->config();
}

void WebrtcPeerSession::set_dtls_transport_state(DtlsTransportState state)
{
    dtls_transport_engine_->set_state(state);
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

DtlsTransportState WebrtcPeerSession::dtls_transport_state() const
{
    return dtls_transport_state_;
}

void WebrtcPeerSession::set_dtls_transport_engine(std::shared_ptr<DtlsTransportEngine> engine)
{
    dtls_transport_engine_ = engine ? std::move(engine) : std::make_shared<MockDtlsTransport>();
    sync_transport_from_mocks(0);
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

void WebrtcPeerSession::set_mock_dtls_transport_config(const MockDtlsTransportConfig &config)
{
    auto mock = std::dynamic_pointer_cast<MockDtlsTransport>(dtls_transport_engine_);
    if (!mock) {
        mock = std::make_shared<MockDtlsTransport>();
        dtls_transport_engine_ = mock;
    }
    mock->set_config(config);
}

MockDtlsTransportConfig WebrtcPeerSession::mock_dtls_transport_config() const
{
    const auto mock = std::dynamic_pointer_cast<MockDtlsTransport>(dtls_transport_engine_);
    if (!mock) {
        return {};
    }
    return mock->config();
}

void WebrtcPeerSession::advance_transport(uint64_t now_ms)
{
    sync_transport_from_mocks(now_ms);
    refresh_scheduler_activation(now_ms);
    refresh_connection_state();
}

bool WebrtcPeerSession::is_transport_ready() const
{
    return ice_transport_state_ == IceTransportState::connected
           && dtls_transport_state_ == DtlsTransportState::connected;
}

void WebrtcPeerSession::set_require_dtls_fingerprint_match_for_media(bool required)
{
    require_dtls_fingerprint_match_for_media_ = required;
    refresh_scheduler_activation(0);
    refresh_connection_state();
}

bool WebrtcPeerSession::require_dtls_fingerprint_match_for_media() const
{
    return require_dtls_fingerprint_match_for_media_;
}

const std::string &WebrtcPeerSession::last_security_error() const
{
    return last_security_error_;
}

SecurityErrorCode WebrtcPeerSession::last_security_error_code() const
{
    return last_security_error_code_;
}

bool WebrtcPeerSession::has_dtls_srtp_keying_material() const
{
    return dtls_transport_engine_ && dtls_transport_engine_->has_srtp_keying_material();
}

DtlsSrtpKeyingMaterial WebrtcPeerSession::dtls_srtp_keying_material() const
{
    if (!dtls_transport_engine_) {
        return {};
    }
    return dtls_transport_engine_->srtp_keying_material();
}

DtlsFingerprintVerificationState WebrtcPeerSession::dtls_fingerprint_verification_state() const
{
    if (!dtls_transport_engine_) {
        return DtlsFingerprintVerificationState::unknown;
    }
    return dtls_transport_engine_->fingerprint_verification_state();
}

DtlsPeerFingerprint WebrtcPeerSession::dtls_peer_fingerprint() const
{
    if (!dtls_transport_engine_) {
        return {};
    }
    return dtls_transport_engine_->peer_fingerprint();
}

bool WebrtcPeerSession::is_dtls_fingerprint_consistent_with_remote_sdp() const
{
    if (!dtls_transport_engine_ || !signaling_bridge_.has_parsed_remote_sdp()) {
        return false;
    }
    const auto &remote_sdp = signaling_bridge_.parsed_remote_sdp();
    if (!remote_sdp.has_fingerprint) {
        return false;
    }

    const DtlsPeerFingerprint fp = dtls_transport_engine_->peer_fingerprint();
    if (fp.algorithm.empty() || fp.value.empty()) {
        return false;
    }
    return fp.algorithm == remote_sdp.fingerprint.algorithm && fp.value == remote_sdp.fingerprint.value;
}

bool WebrtcPeerSession::is_media_ready() const
{
    if (!injected_security_error_.empty()) {
        return false;
    }
    std::string error;
    if (!security_gate_passed(&error)) {
        return false;
    }
    return signaling_bridge_.signaling_state() == SignalingState::stable && is_transport_ready();
}

PeerConnectionState WebrtcPeerSession::connection_state() const
{
    return connection_state_;
}

void WebrtcPeerSession::set_connection_state_callback(ConnectionStateCallback callback)
{
    connection_state_callback_ = std::move(callback);
}

WebrtcPeerSessionSnapshot WebrtcPeerSession::snapshot() const
{
    WebrtcPeerSessionSnapshot out;
    out.signaling_state = signaling_state();
    out.ice_transport_state = ice_transport_state();
    out.dtls_transport_state = dtls_transport_state();
    out.connection_state = connection_state();
    out.has_local_description = signaling_bridge_.has_local_description();
    out.has_remote_description = signaling_bridge_.has_remote_description();
    out.remote_candidate_count = signaling_bridge_.remote_candidates().size();
    out.ice_gathering_state = ice_gathering_state();
    out.ice_checklist_state = ice_checklist_state();
    out.ice_nomination_state = ice_transport_engine_ ? ice_transport_engine_->nomination_state() : IceNominationState::none;
    out.has_selected_ice_pair = has_selected_ice_pair();
    if (out.has_selected_ice_pair) {
        const IceCandidatePair pair = selected_ice_pair();
        out.selected_ice_pair_reason = pair.reason;
        out.selected_ice_pair_reason_text = pair.reason_text;
        out.selected_ice_pair_nomination_transaction_id = pair.nomination_transaction_id;
    }
    out.last_ice_error = last_ice_error_;
    out.stun_transactions = ice_transport_engine_ ? ice_transport_engine_->stun_transactions() : std::vector<StunTransaction>{};
    out.has_last_stun_transaction = ice_transport_engine_ && ice_transport_engine_->has_last_stun_transaction();
    if (out.has_last_stun_transaction) {
        out.last_stun_transaction = ice_transport_engine_->last_stun_transaction();
    }
    out.pending_ice_nomination_signal_count = pending_ice_nomination_signals_.size();
    out.peak_pending_ice_nomination_signal_count = peak_pending_ice_nomination_signal_count_;
    out.dropped_ice_nomination_signal_count = dropped_ice_nomination_signal_count_;
    out.dropped_ice_nomination_signal_overflow_count = dropped_ice_nomination_signal_overflow_count_;
    out.dropped_ice_nomination_signal_trim_count = dropped_ice_nomination_signal_trim_count_;
    out.pending_diagnostics_signal_count = pending_diagnostics_signals_.size();
    out.peak_pending_diagnostics_signal_count = peak_pending_diagnostics_signal_count_;
    out.dropped_diagnostics_signal_count = dropped_diagnostics_signal_count_;
    out.dropped_diagnostics_signal_overflow_count = dropped_diagnostics_signal_overflow_count_;
    out.dropped_diagnostics_signal_trim_count = dropped_diagnostics_signal_trim_count_;
    out.diagnostics_v2_flat_duplicate_seen_count = signaling_bridge_.diagnostics_v2_flat_duplicate_seen_count();
    out.diagnostics_v2_flat_duplicate_mismatch_count =
        signaling_bridge_.diagnostics_v2_flat_duplicate_mismatch_count();
    out.diagnostics_emit_flat_compat_fields =
        signaling_bridge_.diagnostics_emission_config().emit_flat_compat_fields;
    out.diagnostics_release_mode_strict_v2 =
        signaling_bridge_.diagnostics_emission_config().release_mode_strict_v2;
    out.diagnostics_rollout_alert_window_seconds =
        signaling_bridge_.diagnostics_emission_config().rollout_alert_window_seconds;
    out.diagnostics_rollout_mismatch_count_alert_threshold =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_count_alert_threshold;
    out.diagnostics_rollout_mismatch_ratio_threshold_ppm =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_ratio_threshold_ppm;
    const uint64_t seen = out.diagnostics_v2_flat_duplicate_seen_count;
    const uint64_t mismatch = out.diagnostics_v2_flat_duplicate_mismatch_count;
    out.diagnostics_rollout_current_mismatch_ratio_ppm =
        (seen == 0)
            ? 0
            : static_cast<uint64_t>((static_cast<long double>(mismatch) * 1000000.0L)
                                    / static_cast<long double>(seen));
    out.diagnostics_rollout_alert_active =
        mismatch > out.diagnostics_rollout_mismatch_count_alert_threshold;
    out.diagnostics_rollout_progress_blocked =
        out.diagnostics_rollout_alert_active
        || out.diagnostics_rollout_current_mismatch_ratio_ppm
               > out.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    out.diagnostics_rollout_ready_for_progress = !out.diagnostics_rollout_progress_blocked;
    out.diagnostics_policy_keep_latest_only = diagnostics_signal_queue_config_.keep_latest_only;
    out.diagnostics_policy_max_pending_signals = diagnostics_signal_queue_config_.max_pending_signals;
    out.diagnostics_policy_nomination_max_pending_signals = nomination_signal_queue_config_.max_pending_signals;
    out.transport_ready = is_transport_ready();
    out.media_ready = is_media_ready();
    out.scheduler_active = scheduler_active_;
    out.dtls_fingerprint_consistent = is_dtls_fingerprint_consistent_with_remote_sdp();
    out.dtls_fingerprint_policy_required = require_dtls_fingerprint_match_for_media_;
    out.security_error = last_security_error_;
    out.security_error_code = last_security_error_code_;
    return out;
}

std::string WebrtcPeerSession::snapshot_json() const
{
    const auto s = snapshot();
    Json root;
    root["signaling_state"] = to_signaling_state_text(s.signaling_state);
    root["ice_transport_state"] = to_ice_state_text(s.ice_transport_state);
    root["dtls_transport_state"] = to_dtls_state_text(s.dtls_transport_state);
    root["connection_state"] = to_connection_state_text(s.connection_state);
    root["has_local_description"] = s.has_local_description;
    root["has_remote_description"] = s.has_remote_description;
    root["remote_candidate_count"] = s.remote_candidate_count;
    root["ice_gathering_state"] = static_cast<int>(s.ice_gathering_state);
    root["ice_checklist_state"] = static_cast<int>(s.ice_checklist_state);
    root["ice_nomination_state"] = static_cast<int>(s.ice_nomination_state);
    root["has_selected_ice_pair"] = s.has_selected_ice_pair;
    root["selected_ice_pair_reason"] = static_cast<int>(s.selected_ice_pair_reason);
    root["selected_ice_pair_reason_text"] = s.selected_ice_pair_reason_text;
    root["selected_ice_pair_nomination_transaction_id"] = s.selected_ice_pair_nomination_transaction_id;
    root["last_ice_error"] = s.last_ice_error;
    root["stun_transactions"] = Json::array();
    for (const auto &tx : s.stun_transactions) {
        Json tx_json;
        tx_json["transaction_id"] = tx.transaction_id;
        tx_json["state"] = to_stun_transaction_state_text(tx.state);
        tx_json["started_at_ms"] = tx.started_at_ms;
        tx_json["last_request_at_ms"] = tx.last_request_at_ms;
        tx_json["completed_at_ms"] = tx.completed_at_ms;
        tx_json["request_count"] = tx.request_count;
        tx_json["retransmit_count"] = tx.retransmit_count;
        tx_json["request_priority"] = tx.request_priority;
        tx_json["request_use_candidate"] = tx.request_use_candidate;
        tx_json["error"] = tx.error;
        tx_json["response_code"] = tx.response_code;
        tx_json["mapped_address"] = tx.mapped_address;
        tx_json["mapped_port"] = tx.mapped_port;
        root["stun_transactions"].push_back(std::move(tx_json));
    }
    root["has_last_stun_transaction"] = s.has_last_stun_transaction;
    Json last_tx_json;
    last_tx_json["transaction_id"] = s.last_stun_transaction.transaction_id;
    last_tx_json["state"] = to_stun_transaction_state_text(s.last_stun_transaction.state);
    last_tx_json["started_at_ms"] = s.last_stun_transaction.started_at_ms;
    last_tx_json["last_request_at_ms"] = s.last_stun_transaction.last_request_at_ms;
    last_tx_json["completed_at_ms"] = s.last_stun_transaction.completed_at_ms;
    last_tx_json["request_count"] = s.last_stun_transaction.request_count;
    last_tx_json["retransmit_count"] = s.last_stun_transaction.retransmit_count;
    last_tx_json["request_priority"] = s.last_stun_transaction.request_priority;
    last_tx_json["request_use_candidate"] = s.last_stun_transaction.request_use_candidate;
    last_tx_json["error"] = s.last_stun_transaction.error;
    last_tx_json["response_code"] = s.last_stun_transaction.response_code;
    last_tx_json["mapped_address"] = s.last_stun_transaction.mapped_address;
    last_tx_json["mapped_port"] = s.last_stun_transaction.mapped_port;
    root["last_stun_transaction"] = std::move(last_tx_json);
    root["pending_ice_nomination_signal_count"] = s.pending_ice_nomination_signal_count;
    root["peak_pending_ice_nomination_signal_count"] = s.peak_pending_ice_nomination_signal_count;
    root["dropped_ice_nomination_signal_count"] = s.dropped_ice_nomination_signal_count;
    root["dropped_ice_nomination_signal_overflow_count"] = s.dropped_ice_nomination_signal_overflow_count;
    root["dropped_ice_nomination_signal_trim_count"] = s.dropped_ice_nomination_signal_trim_count;
    root["pending_diagnostics_signal_count"] = s.pending_diagnostics_signal_count;
    root["peak_pending_diagnostics_signal_count"] = s.peak_pending_diagnostics_signal_count;
    root["dropped_diagnostics_signal_count"] = s.dropped_diagnostics_signal_count;
    root["dropped_diagnostics_signal_overflow_count"] = s.dropped_diagnostics_signal_overflow_count;
    root["dropped_diagnostics_signal_trim_count"] = s.dropped_diagnostics_signal_trim_count;
    root["diagnostics_v2_flat_duplicate_seen_count"] = s.diagnostics_v2_flat_duplicate_seen_count;
    root["diagnostics_v2_flat_duplicate_mismatch_count"] = s.diagnostics_v2_flat_duplicate_mismatch_count;
    root["diagnostics_emit_flat_compat_fields"] = s.diagnostics_emit_flat_compat_fields;
    root["diagnostics_release_mode_strict_v2"] = s.diagnostics_release_mode_strict_v2;
    root["diagnostics_rollout_alert_window_seconds"] = s.diagnostics_rollout_alert_window_seconds;
    root["diagnostics_rollout_mismatch_count_alert_threshold"] =
        s.diagnostics_rollout_mismatch_count_alert_threshold;
    root["diagnostics_rollout_mismatch_ratio_threshold_ppm"] =
        s.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    root["diagnostics_rollout_current_mismatch_ratio_ppm"] =
        s.diagnostics_rollout_current_mismatch_ratio_ppm;
    root["diagnostics_rollout_alert_active"] = s.diagnostics_rollout_alert_active;
    root["diagnostics_rollout_progress_blocked"] = s.diagnostics_rollout_progress_blocked;
    root["diagnostics_rollout_ready_for_progress"] = s.diagnostics_rollout_ready_for_progress;
    root["diagnostics_policy_keep_latest_only"] = s.diagnostics_policy_keep_latest_only;
    root["diagnostics_policy_max_pending_signals"] = s.diagnostics_policy_max_pending_signals;
    root["diagnostics_policy_nomination_max_pending_signals"] = s.diagnostics_policy_nomination_max_pending_signals;
    root["transport_ready"] = s.transport_ready;
    root["media_ready"] = s.media_ready;
    root["scheduler_active"] = s.scheduler_active;
    root["dtls_fingerprint_consistent"] = s.dtls_fingerprint_consistent;
    root["dtls_fingerprint_policy_required"] = s.dtls_fingerprint_policy_required;
    root["security_error"] = s.security_error;
    root["security_error_code"] = static_cast<int>(s.security_error_code);
    return root.dump();
}

bool WebrtcPeerSession::parse_snapshot_json(const std::string &json_text, WebrtcPeerSessionSnapshot &out_snapshot) const
{
    try {
        const Json root = Json::parse(json_text);
        if (!root.is_object()) {
            return false;
        }

        if (!root.contains("signaling_state") || !root["signaling_state"].is_string()) {
            return false;
        }
        if (!root.contains("ice_transport_state") || !root["ice_transport_state"].is_string()) {
            return false;
        }
        if (!root.contains("dtls_transport_state") || !root["dtls_transport_state"].is_string()) {
            return false;
        }
        if (!root.contains("connection_state") || !root["connection_state"].is_string()) {
            return false;
        }
        if (!root.contains("has_local_description") || !root["has_local_description"].is_boolean()) {
            return false;
        }
        if (!root.contains("has_remote_description") || !root["has_remote_description"].is_boolean()) {
            return false;
        }
        if (!root.contains("remote_candidate_count") || !root["remote_candidate_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("ice_gathering_state") || !root["ice_gathering_state"].is_number_integer()) {
            return false;
        }
        if (!root.contains("ice_checklist_state") || !root["ice_checklist_state"].is_number_integer()) {
            return false;
        }
        if (!root.contains("ice_nomination_state") || !root["ice_nomination_state"].is_number_integer()) {
            return false;
        }
        if (!root.contains("has_selected_ice_pair") || !root["has_selected_ice_pair"].is_boolean()) {
            return false;
        }
        if (!root.contains("selected_ice_pair_reason") || !root["selected_ice_pair_reason"].is_number_integer()) {
            return false;
        }
        if (!root.contains("selected_ice_pair_reason_text") || !root["selected_ice_pair_reason_text"].is_string()) {
            return false;
        }
        if (!root.contains("selected_ice_pair_nomination_transaction_id")
            || !root["selected_ice_pair_nomination_transaction_id"].is_string()) {
            return false;
        }
        if (!root.contains("last_ice_error") || !root["last_ice_error"].is_string()) {
            return false;
        }
        if (!root.contains("stun_transactions") || !root["stun_transactions"].is_array()) {
            return false;
        }
        if (!root.contains("has_last_stun_transaction") || !root["has_last_stun_transaction"].is_boolean()) {
            return false;
        }
        if (!root.contains("last_stun_transaction") || !root["last_stun_transaction"].is_object()) {
            return false;
        }
        if (!root.contains("pending_ice_nomination_signal_count")
            || !root["pending_ice_nomination_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("peak_pending_ice_nomination_signal_count")
            || !root["peak_pending_ice_nomination_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_ice_nomination_signal_count")
            || !root["dropped_ice_nomination_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_ice_nomination_signal_overflow_count")
            || !root["dropped_ice_nomination_signal_overflow_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_ice_nomination_signal_trim_count")
            || !root["dropped_ice_nomination_signal_trim_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("pending_diagnostics_signal_count")
            || !root["pending_diagnostics_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("peak_pending_diagnostics_signal_count")
            || !root["peak_pending_diagnostics_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_diagnostics_signal_count")
            || !root["dropped_diagnostics_signal_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_diagnostics_signal_overflow_count")
            || !root["dropped_diagnostics_signal_overflow_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("dropped_diagnostics_signal_trim_count")
            || !root["dropped_diagnostics_signal_trim_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_v2_flat_duplicate_seen_count")
            || !root["diagnostics_v2_flat_duplicate_seen_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_v2_flat_duplicate_mismatch_count")
            || !root["diagnostics_v2_flat_duplicate_mismatch_count"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_emit_flat_compat_fields")
            || !root["diagnostics_emit_flat_compat_fields"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_release_mode_strict_v2")
            || !root["diagnostics_release_mode_strict_v2"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_alert_window_seconds")
            || !root["diagnostics_rollout_alert_window_seconds"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_mismatch_count_alert_threshold")
            || !root["diagnostics_rollout_mismatch_count_alert_threshold"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_mismatch_ratio_threshold_ppm")
            || !root["diagnostics_rollout_mismatch_ratio_threshold_ppm"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_current_mismatch_ratio_ppm")
            || !root["diagnostics_rollout_current_mismatch_ratio_ppm"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_alert_active")
            || !root["diagnostics_rollout_alert_active"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_progress_blocked")
            || !root["diagnostics_rollout_progress_blocked"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_rollout_ready_for_progress")
            || !root["diagnostics_rollout_ready_for_progress"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_policy_keep_latest_only")
            || !root["diagnostics_policy_keep_latest_only"].is_boolean()) {
            return false;
        }
        if (!root.contains("diagnostics_policy_max_pending_signals")
            || !root["diagnostics_policy_max_pending_signals"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("diagnostics_policy_nomination_max_pending_signals")
            || !root["diagnostics_policy_nomination_max_pending_signals"].is_number_unsigned()) {
            return false;
        }
        if (!root.contains("transport_ready") || !root["transport_ready"].is_boolean()) {
            return false;
        }
        if (!root.contains("media_ready") || !root["media_ready"].is_boolean()) {
            return false;
        }
        if (!root.contains("scheduler_active") || !root["scheduler_active"].is_boolean()) {
            return false;
        }
        if (!root.contains("dtls_fingerprint_consistent") || !root["dtls_fingerprint_consistent"].is_boolean()) {
            return false;
        }
        if (!root.contains("dtls_fingerprint_policy_required") || !root["dtls_fingerprint_policy_required"].is_boolean()) {
            return false;
        }
        if (!root.contains("security_error") || !root["security_error"].is_string()) {
            return false;
        }
        if (!root.contains("security_error_code") || !root["security_error_code"].is_number_integer()) {
            return false;
        }

        WebrtcPeerSessionSnapshot parsed;
        if (!parse_signaling_state_text(root["signaling_state"].get<std::string>(), parsed.signaling_state)) {
            return false;
        }
        if (!parse_ice_state_text(root["ice_transport_state"].get<std::string>(), parsed.ice_transport_state)) {
            return false;
        }
        if (!parse_dtls_state_text(root["dtls_transport_state"].get<std::string>(), parsed.dtls_transport_state)) {
            return false;
        }
        if (!parse_connection_state_text(root["connection_state"].get<std::string>(), parsed.connection_state)) {
            return false;
        }

        parsed.has_local_description = root["has_local_description"].get<bool>();
        parsed.has_remote_description = root["has_remote_description"].get<bool>();
        parsed.remote_candidate_count = root["remote_candidate_count"].get<std::size_t>();
        parsed.ice_gathering_state = static_cast<IceGatheringState>(root["ice_gathering_state"].get<int>());
        parsed.ice_checklist_state = static_cast<IceChecklistState>(root["ice_checklist_state"].get<int>());
        parsed.ice_nomination_state = static_cast<IceNominationState>(root["ice_nomination_state"].get<int>());
        parsed.has_selected_ice_pair = root["has_selected_ice_pair"].get<bool>();
        parsed.selected_ice_pair_reason = static_cast<IceSelectedPairReason>(root["selected_ice_pair_reason"].get<int>());
        parsed.selected_ice_pair_reason_text = root["selected_ice_pair_reason_text"].get<std::string>();
        parsed.selected_ice_pair_nomination_transaction_id = root["selected_ice_pair_nomination_transaction_id"].get<std::string>();
        parsed.last_ice_error = root["last_ice_error"].get<std::string>();
        parsed.stun_transactions.clear();
        for (const auto &tx_json : root["stun_transactions"]) {
            if (!tx_json.is_object()) {
                return false;
            }
            if (!tx_json.contains("transaction_id") || !tx_json["transaction_id"].is_string()) {
                return false;
            }
            if (!tx_json.contains("state") || !tx_json["state"].is_string()) {
                return false;
            }
            if (!tx_json.contains("started_at_ms") || !tx_json["started_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("last_request_at_ms") || !tx_json["last_request_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("completed_at_ms") || !tx_json["completed_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_count") || !tx_json["request_count"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("retransmit_count") || !tx_json["retransmit_count"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_priority") || !tx_json["request_priority"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_use_candidate") || !tx_json["request_use_candidate"].is_boolean()) {
                return false;
            }
            if (!tx_json.contains("error") || !tx_json["error"].is_string()) {
                return false;
            }
            if (!tx_json.contains("response_code") || !tx_json["response_code"].is_string()) {
                return false;
            }
            if (!tx_json.contains("mapped_address") || !tx_json["mapped_address"].is_string()) {
                return false;
            }
            if (!tx_json.contains("mapped_port") || !tx_json["mapped_port"].is_number_integer()) {
                return false;
            }

            StunTransaction tx;
            tx.transaction_id = tx_json["transaction_id"].get<std::string>();
            if (!parse_stun_transaction_state_text(tx_json["state"].get<std::string>(), tx.state)) {
                return false;
            }
            tx.started_at_ms = tx_json["started_at_ms"].get<uint64_t>();
            tx.last_request_at_ms = tx_json["last_request_at_ms"].get<uint64_t>();
            tx.completed_at_ms = tx_json["completed_at_ms"].get<uint64_t>();
            tx.request_count = tx_json["request_count"].get<uint32_t>();
            tx.retransmit_count = tx_json["retransmit_count"].get<uint32_t>();
            tx.request_priority = tx_json["request_priority"].get<uint32_t>();
            tx.request_use_candidate = tx_json["request_use_candidate"].get<bool>();
            tx.error = tx_json["error"].get<std::string>();
            tx.response_code = tx_json["response_code"].get<std::string>();
            tx.mapped_address = tx_json["mapped_address"].get<std::string>();
            tx.mapped_port = tx_json["mapped_port"].get<uint16_t>();
            parsed.stun_transactions.push_back(std::move(tx));
        }
        parsed.has_last_stun_transaction = root["has_last_stun_transaction"].get<bool>();
        {
            const Json &tx_json = root["last_stun_transaction"];
            if (!tx_json.contains("transaction_id") || !tx_json["transaction_id"].is_string()) {
                return false;
            }
            if (!tx_json.contains("state") || !tx_json["state"].is_string()) {
                return false;
            }
            if (!tx_json.contains("started_at_ms") || !tx_json["started_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("last_request_at_ms") || !tx_json["last_request_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("completed_at_ms") || !tx_json["completed_at_ms"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_count") || !tx_json["request_count"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("retransmit_count") || !tx_json["retransmit_count"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_priority") || !tx_json["request_priority"].is_number_unsigned()) {
                return false;
            }
            if (!tx_json.contains("request_use_candidate") || !tx_json["request_use_candidate"].is_boolean()) {
                return false;
            }
            if (!tx_json.contains("error") || !tx_json["error"].is_string()) {
                return false;
            }
            if (!tx_json.contains("response_code") || !tx_json["response_code"].is_string()) {
                return false;
            }
            if (!tx_json.contains("mapped_address") || !tx_json["mapped_address"].is_string()) {
                return false;
            }
            if (!tx_json.contains("mapped_port") || !tx_json["mapped_port"].is_number_integer()) {
                return false;
            }
            parsed.last_stun_transaction.transaction_id = tx_json["transaction_id"].get<std::string>();
            if (!parse_stun_transaction_state_text(tx_json["state"].get<std::string>(), parsed.last_stun_transaction.state)) {
                return false;
            }
            parsed.last_stun_transaction.started_at_ms = tx_json["started_at_ms"].get<uint64_t>();
            parsed.last_stun_transaction.last_request_at_ms = tx_json["last_request_at_ms"].get<uint64_t>();
            parsed.last_stun_transaction.completed_at_ms = tx_json["completed_at_ms"].get<uint64_t>();
            parsed.last_stun_transaction.request_count = tx_json["request_count"].get<uint32_t>();
            parsed.last_stun_transaction.retransmit_count = tx_json["retransmit_count"].get<uint32_t>();
            parsed.last_stun_transaction.request_priority = tx_json["request_priority"].get<uint32_t>();
            parsed.last_stun_transaction.request_use_candidate = tx_json["request_use_candidate"].get<bool>();
            parsed.last_stun_transaction.error = tx_json["error"].get<std::string>();
            parsed.last_stun_transaction.response_code = tx_json["response_code"].get<std::string>();
            parsed.last_stun_transaction.mapped_address = tx_json["mapped_address"].get<std::string>();
            parsed.last_stun_transaction.mapped_port = tx_json["mapped_port"].get<uint16_t>();
        }
        parsed.pending_ice_nomination_signal_count = root["pending_ice_nomination_signal_count"].get<std::size_t>();
        parsed.peak_pending_ice_nomination_signal_count =
            root["peak_pending_ice_nomination_signal_count"].get<std::size_t>();
        parsed.dropped_ice_nomination_signal_count = root["dropped_ice_nomination_signal_count"].get<uint64_t>();
        parsed.dropped_ice_nomination_signal_overflow_count =
            root["dropped_ice_nomination_signal_overflow_count"].get<uint64_t>();
        parsed.dropped_ice_nomination_signal_trim_count =
            root["dropped_ice_nomination_signal_trim_count"].get<uint64_t>();
        parsed.pending_diagnostics_signal_count = root["pending_diagnostics_signal_count"].get<std::size_t>();
        parsed.peak_pending_diagnostics_signal_count = root["peak_pending_diagnostics_signal_count"].get<std::size_t>();
        parsed.dropped_diagnostics_signal_count = root["dropped_diagnostics_signal_count"].get<uint64_t>();
        parsed.dropped_diagnostics_signal_overflow_count =
            root["dropped_diagnostics_signal_overflow_count"].get<uint64_t>();
        parsed.dropped_diagnostics_signal_trim_count =
            root["dropped_diagnostics_signal_trim_count"].get<uint64_t>();
        parsed.diagnostics_v2_flat_duplicate_seen_count =
            root["diagnostics_v2_flat_duplicate_seen_count"].get<uint64_t>();
        parsed.diagnostics_v2_flat_duplicate_mismatch_count =
            root["diagnostics_v2_flat_duplicate_mismatch_count"].get<uint64_t>();
        parsed.diagnostics_emit_flat_compat_fields =
            root["diagnostics_emit_flat_compat_fields"].get<bool>();
        parsed.diagnostics_release_mode_strict_v2 =
            root["diagnostics_release_mode_strict_v2"].get<bool>();
        parsed.diagnostics_rollout_alert_window_seconds =
            root["diagnostics_rollout_alert_window_seconds"].get<uint64_t>();
        parsed.diagnostics_rollout_mismatch_count_alert_threshold =
            root["diagnostics_rollout_mismatch_count_alert_threshold"].get<uint64_t>();
        parsed.diagnostics_rollout_mismatch_ratio_threshold_ppm =
            root["diagnostics_rollout_mismatch_ratio_threshold_ppm"].get<uint64_t>();
        parsed.diagnostics_rollout_current_mismatch_ratio_ppm =
            root["diagnostics_rollout_current_mismatch_ratio_ppm"].get<uint64_t>();
        parsed.diagnostics_rollout_alert_active =
            root["diagnostics_rollout_alert_active"].get<bool>();
        parsed.diagnostics_rollout_progress_blocked =
            root["diagnostics_rollout_progress_blocked"].get<bool>();
        parsed.diagnostics_rollout_ready_for_progress =
            root["diagnostics_rollout_ready_for_progress"].get<bool>();
        parsed.diagnostics_policy_keep_latest_only = root["diagnostics_policy_keep_latest_only"].get<bool>();
        parsed.diagnostics_policy_max_pending_signals = root["diagnostics_policy_max_pending_signals"].get<std::size_t>();
        parsed.diagnostics_policy_nomination_max_pending_signals =
            root["diagnostics_policy_nomination_max_pending_signals"].get<std::size_t>();
        parsed.transport_ready = root["transport_ready"].get<bool>();
        parsed.media_ready = root["media_ready"].get<bool>();
        parsed.scheduler_active = root["scheduler_active"].get<bool>();
        parsed.dtls_fingerprint_consistent = root["dtls_fingerprint_consistent"].get<bool>();
        parsed.dtls_fingerprint_policy_required = root["dtls_fingerprint_policy_required"].get<bool>();
        parsed.security_error = root["security_error"].get<std::string>();
        const int security_code = root["security_error_code"].get<int>();
        if (security_code < static_cast<int>(SecurityErrorCode::none)
            || security_code > static_cast<int>(SecurityErrorCode::external_security_error)) {
            return false;
        }
        parsed.security_error_code = static_cast<SecurityErrorCode>(security_code);

        if (parsed.security_error.empty()) {
            parsed.security_error = to_security_error_text(parsed.security_error_code);
        }

        out_snapshot = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

SignalingState WebrtcPeerSession::signaling_state() const
{
    return signaling_bridge_.signaling_state();
}

void WebrtcPeerSession::refresh_scheduler_activation(uint64_t now_ms)
{
    const bool should_be_active = is_media_ready();
    if (should_be_active && !scheduler_active_) {
        transport_bridge_.reset_rtcp_schedule(now_ms);
    }
    scheduler_active_ = should_be_active;
}

void WebrtcPeerSession::refresh_connection_state()
{
    if (!injected_security_error_.empty()) {
        last_security_error_ = injected_security_error_;
        last_security_error_code_ = infer_security_error_code(last_security_error_);
        update_connection_state(PeerConnectionState::failed);
        return;
    }

    std::string security_error;
    if (!security_gate_passed(&security_error)) {
        last_security_error_ = security_error;
        last_security_error_code_ = infer_security_error_code(last_security_error_);
        if (signaling_bridge_.signaling_state() == SignalingState::stable && is_transport_ready()) {
            update_connection_state(PeerConnectionState::failed);
            return;
        }
    } else {
        last_security_error_.clear();
        last_security_error_code_ = SecurityErrorCode::none;
    }

    if (ice_transport_state_ == IceTransportState::failed || dtls_transport_state_ == DtlsTransportState::failed) {
        update_connection_state(PeerConnectionState::failed);
        return;
    }

    if (is_media_ready()) {
        update_connection_state(PeerConnectionState::connected);
        return;
    }

    if (signaling_bridge_.signaling_state() == SignalingState::new_
        && ice_transport_state_ == IceTransportState::new_
        && dtls_transport_state_ == DtlsTransportState::new_) {
        update_connection_state(PeerConnectionState::new_);
        return;
    }

    update_connection_state(PeerConnectionState::connecting);
}

void WebrtcPeerSession::update_connection_state(PeerConnectionState next_state)
{
    if (next_state == connection_state_) {
        return;
    }

    const PeerConnectionState previous = connection_state_;
    connection_state_ = next_state;
    if (connection_state_callback_) {
        connection_state_callback_(previous, connection_state_);
    }
}

void WebrtcPeerSession::sync_transport_from_mocks(uint64_t now_ms)
{
    if (!ice_transport_engine_) {
        ice_transport_engine_ = std::make_shared<MockIceTransport>();
    }
    if (!dtls_transport_engine_) {
        dtls_transport_engine_ = std::make_shared<MockDtlsTransport>();
    }

    ice_transport_engine_->poll(now_ms);
    ice_transport_state_ = ice_transport_engine_->state();
    last_ice_error_ = ice_transport_engine_->last_error();
    dtls_transport_engine_->on_ice_state_changed(ice_transport_state_, now_ms);
    dtls_transport_engine_->poll(now_ms, ice_transport_state_);
    dtls_transport_state_ = dtls_transport_engine_->state();
    if (transport_bridge_.has_srtp_context() && dtls_transport_engine_->has_srtp_keying_material()) {
        std::shared_ptr<SrtpContext> ctx = transport_bridge_.srtp_context();
        if (ctx && !ctx->has_keying_material()) {
            (void) ctx->apply_keying_material(dtls_transport_engine_->srtp_keying_material());
        }
    }
    refresh_ice_nomination_signal();
}

void WebrtcPeerSession::refresh_ice_nomination_signal()
{
    if (!ice_transport_engine_) {
        return;
    }

    const IceNominationState current_state = ice_transport_engine_->nomination_state();
    std::string current_transaction_id;
    std::string current_reason_text;
    IceSelectedPairReason current_reason = IceSelectedPairReason::none;
    if (ice_transport_engine_->has_selected_pair()) {
        const IceCandidatePair pair = ice_transport_engine_->selected_pair();
        current_reason = pair.reason;
        current_reason_text = pair.reason_text;
        current_transaction_id = pair.nomination_transaction_id;
    }

    if (current_state == last_notified_nomination_state_
        && current_transaction_id == last_notified_nomination_transaction_id_) {
        return;
    }

    WebrtcSignalingBridge::SignalingMessage msg;
    msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::ice_nomination;
    msg.ice_nomination_state = current_state;
    msg.selected_pair_reason = current_reason;
    msg.selected_pair_reason_text = current_reason_text;
    msg.nomination_transaction_id = current_transaction_id;
    if (pending_ice_nomination_signals_.size() >= nomination_signal_queue_config_.max_pending_signals) {
        pending_ice_nomination_signals_.pop_front();
        ++dropped_ice_nomination_signal_count_;
        ++dropped_ice_nomination_signal_overflow_count_;
    }
    pending_ice_nomination_signals_.push_back(msg);
    if (pending_ice_nomination_signals_.size() > peak_pending_ice_nomination_signal_count_) {
        peak_pending_ice_nomination_signal_count_ = pending_ice_nomination_signals_.size();
    }

    WebrtcSignalingBridge::SignalingMessage diagnostics;
    diagnostics.kind = WebrtcSignalingBridge::SignalingMessage::Kind::diagnostics;
    diagnostics.diagnostics_scope = "peer_session";
    diagnostics.diagnostics_ice_nomination_state = current_state;
    diagnostics.diagnostics_has_selected_pair = ice_transport_engine_->has_selected_pair();
    diagnostics.diagnostics_selected_pair_reason = current_reason;
    diagnostics.diagnostics_selected_pair_reason_text = current_reason_text;
    diagnostics.diagnostics_last_ice_error = last_ice_error_;
    diagnostics.diagnostics_stun_transaction_count = static_cast<uint64_t>(ice_transport_engine_->stun_transactions().size());
    diagnostics.diagnostics_has_last_stun_transaction = ice_transport_engine_->has_last_stun_transaction();
    if (diagnostics.diagnostics_has_last_stun_transaction) {
        const StunTransaction tx = ice_transport_engine_->last_stun_transaction();
        diagnostics.diagnostics_last_stun_transaction_id = tx.transaction_id;
        diagnostics.diagnostics_last_stun_transaction_state = tx.state;
    }
    diagnostics.diagnostics_pending_nomination_signal_count =
        static_cast<uint64_t>(pending_ice_nomination_signals_.size());
    diagnostics.diagnostics_dropped_nomination_signal_count = dropped_ice_nomination_signal_count_;
    diagnostics.diagnostics_dropped_nomination_signal_overflow_count =
        dropped_ice_nomination_signal_overflow_count_;
    diagnostics.diagnostics_dropped_nomination_signal_trim_count =
        dropped_ice_nomination_signal_trim_count_;
    diagnostics.diagnostics_pending_diagnostics_signal_count =
        static_cast<uint64_t>(pending_diagnostics_signals_.size());
    diagnostics.diagnostics_dropped_diagnostics_signal_count = dropped_diagnostics_signal_count_;
    diagnostics.diagnostics_dropped_diagnostics_signal_overflow_count =
        dropped_diagnostics_signal_overflow_count_;
    diagnostics.diagnostics_dropped_diagnostics_signal_trim_count =
        dropped_diagnostics_signal_trim_count_;
    diagnostics.diagnostics_v2_flat_duplicate_seen_count =
        signaling_bridge_.diagnostics_v2_flat_duplicate_seen_count();
    diagnostics.diagnostics_v2_flat_duplicate_mismatch_count =
        signaling_bridge_.diagnostics_v2_flat_duplicate_mismatch_count();
    diagnostics.diagnostics_emit_flat_compat_fields =
        signaling_bridge_.diagnostics_emission_config().emit_flat_compat_fields;
    diagnostics.diagnostics_release_mode_strict_v2 =
        signaling_bridge_.diagnostics_emission_config().release_mode_strict_v2;
    diagnostics.diagnostics_rollout_alert_window_seconds =
        signaling_bridge_.diagnostics_emission_config().rollout_alert_window_seconds;
    diagnostics.diagnostics_rollout_mismatch_count_alert_threshold =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_count_alert_threshold;
    diagnostics.diagnostics_rollout_mismatch_ratio_threshold_ppm =
        signaling_bridge_.diagnostics_emission_config().rollout_mismatch_ratio_threshold_ppm;
    diagnostics.diagnostics_rollout_current_mismatch_ratio_ppm =
        (diagnostics.diagnostics_v2_flat_duplicate_seen_count == 0)
            ? 0
            : static_cast<uint64_t>(
                  (static_cast<long double>(diagnostics.diagnostics_v2_flat_duplicate_mismatch_count)
                   * 1000000.0L)
                  / static_cast<long double>(diagnostics.diagnostics_v2_flat_duplicate_seen_count));
    diagnostics.diagnostics_rollout_alert_active =
        diagnostics.diagnostics_v2_flat_duplicate_mismatch_count
        > diagnostics.diagnostics_rollout_mismatch_count_alert_threshold;
    diagnostics.diagnostics_rollout_progress_blocked =
        diagnostics.diagnostics_rollout_alert_active
        || diagnostics.diagnostics_rollout_current_mismatch_ratio_ppm
               > diagnostics.diagnostics_rollout_mismatch_ratio_threshold_ppm;
    diagnostics.diagnostics_rollout_ready_for_progress =
        !diagnostics.diagnostics_rollout_progress_blocked;
    diagnostics.diagnostics_policy_keep_latest_only = diagnostics_signal_queue_config_.keep_latest_only;
    diagnostics.diagnostics_policy_max_pending_signals =
        static_cast<uint64_t>(diagnostics_signal_queue_config_.max_pending_signals);
    diagnostics.diagnostics_policy_nomination_max_pending_signals =
        static_cast<uint64_t>(nomination_signal_queue_config_.max_pending_signals);
    if (diagnostics_signal_queue_config_.keep_latest_only) {
        if (!pending_diagnostics_signals_.empty()) {
            dropped_diagnostics_signal_count_ += pending_diagnostics_signals_.size();
            dropped_diagnostics_signal_overflow_count_ += pending_diagnostics_signals_.size();
            pending_diagnostics_signals_.clear();
        }
        pending_diagnostics_signals_.push_back(std::move(diagnostics));
    } else {
        if (pending_diagnostics_signals_.size() >= diagnostics_signal_queue_config_.max_pending_signals) {
            pending_diagnostics_signals_.pop_front();
            ++dropped_diagnostics_signal_count_;
            ++dropped_diagnostics_signal_overflow_count_;
        }
        pending_diagnostics_signals_.push_back(std::move(diagnostics));
    }
    if (pending_diagnostics_signals_.size() > peak_pending_diagnostics_signal_count_) {
        peak_pending_diagnostics_signal_count_ = pending_diagnostics_signals_.size();
    }

    last_notified_nomination_state_ = current_state;
    last_notified_nomination_transaction_id_ = current_transaction_id;
}

bool WebrtcPeerSession::security_gate_passed(std::string *out_error) const
{
    if (!require_dtls_fingerprint_match_for_media_) {
        if (out_error) {
            out_error->clear();
        }
        return true;
    }

    if (!is_dtls_fingerprint_consistent_with_remote_sdp()) {
        if (out_error) {
            *out_error = "dtls_fingerprint_mismatch";
        }
        return false;
    }

    if (out_error) {
        out_error->clear();
    }
    return true;
}

} // namespace yuan::net::webrtc
