#include "webrtc.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using namespace yuan::net::rtc;
using namespace yuan::net::rtcp;
using namespace yuan::net::webrtc;

namespace
{

int g_run = 0;
int g_pass = 0;
int g_fail = 0;

#define TEST_ASSERT(expr, msg)                                                                  \
    do {                                                                                         \
        if (!(expr)) {                                                                           \
            std::cout << "  FAIL: " << msg << " (line " << __LINE__ << ")\n";             \
            return false;                                                                        \
        }                                                                                        \
    } while (0)

#define RUN_TEST(func)                                                                           \
    do {                                                                                         \
        ++g_run;                                                                                 \
        std::cout << "Running " #func "...\n";                                               \
        if (func()) {                                                                            \
            ++g_pass;                                                                            \
            std::cout << "  PASS\n";                                                           \
        } else {                                                                                 \
            ++g_fail;                                                                            \
            std::cout << "  FAIL\n";                                                           \
        }                                                                                        \
    } while (0)

RtcPacket make_rtp(uint32_t ssrc, uint16_t seq, uint32_t ts)
{
    RtcPacket pkt;
    pkt.ssrc = ssrc;
    pkt.sequence_number = seq;
    pkt.timestamp = ts;
    pkt.payload = {0x11, 0x22, 0x33};
    return pkt;
}

std::string valid_offer_sdp()
{
    return "v=0\r\n"
           "o=- 1 1 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "a=group:BUNDLE 0\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "a=mid:0\r\n"
           "a=sendrecv\r\n"
           "a=rtpmap:111 opus/48000/2\r\n";
}

std::string valid_answer_sdp()
{
    return "v=0\r\n"
           "o=- 2 2 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "a=group:BUNDLE 0\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "a=mid:0\r\n"
           "a=sendrecv\r\n"
           "a=rtpmap:111 opus/48000/2\r\n";
}

std::string valid_offer_json()
{
    return "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 1 IN IP4 127.0.0.1\\r\\ns=-\\r\\nt=0 0\\r\\na=group:BUNDLE 0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\na=mid:0\\r\\na=sendrecv\\r\\na=rtpmap:111 opus/48000/2\\r\\n\"}";
}

std::string valid_answer_json()
{
    return "{\"type\":\"answer\",\"sdp\":\"v=0\\r\\no=- 2 2 IN IP4 127.0.0.1\\r\\ns=-\\r\\nt=0 0\\r\\na=group:BUNDLE 0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\na=mid:0\\r\\na=sendrecv\\r\\na=rtpmap:111 opus/48000/2\\r\\n\"}";
}

std::string offer_two_media_sdp()
{
    return "v=0\r\n"
           "o=- 10 10 IN IP4 127.0.0.1\r\n"
           "s=-\r\n"
           "t=0 0\r\n"
           "a=group:BUNDLE 0 1\r\n"
           "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
           "a=mid:0\r\n"
           "a=sendrecv\r\n"
           "a=rtpmap:111 opus/48000/2\r\n"
           "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
           "a=mid:1\r\n"
           "a=sendrecv\r\n"
           "a=rtpmap:96 VP8/90000\r\n";
}

bool test_webrtc_signaling_bridge_basics()
{
    WebrtcSignalingBridge bridge;

    SessionDescription offer;
    offer.type = SdpType::offer;
    offer.sdp = valid_offer_sdp();

    SessionDescription answer;
    answer.type = SdpType::answer;
    answer.sdp = valid_answer_sdp();

    IceCandidate candidate;
    candidate.candidate = "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host";
    candidate.mid = "0";
    candidate.mline_index = 0;

    TEST_ASSERT(!bridge.has_local_description(), "local description should be empty initially");
    TEST_ASSERT(!bridge.has_remote_description(), "remote description should be empty initially");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::new_, "initial state should be new");
    TEST_ASSERT(bridge.set_local_description(offer), "set_local_description should accept non-empty SDP");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::have_local_offer, "state should move to have_local_offer");
    TEST_ASSERT(bridge.has_parsed_local_sdp(), "local parsed SDP should be available");
    TEST_ASSERT(!bridge.parsed_local_sdp().media_sections.empty(), "parsed local SDP should include media sections");
    TEST_ASSERT(bridge.set_remote_description(answer), "set_remote_description should accept non-empty SDP");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::stable, "state should move to stable after answer");
    TEST_ASSERT(bridge.has_parsed_remote_sdp(), "remote parsed SDP should be available");
    TEST_ASSERT(bridge.last_sdp_error().empty(), "last SDP error should be empty after success");
    TEST_ASSERT(bridge.add_remote_candidate(candidate), "add_remote_candidate should accept non-empty candidate");
    TEST_ASSERT(bridge.has_local_description(), "local description should be set");
    TEST_ASSERT(bridge.has_remote_description(), "remote description should be set");
    TEST_ASSERT(bridge.local_description().type == SdpType::offer, "local description type should be offer");
    TEST_ASSERT(bridge.remote_description().type == SdpType::answer, "remote description type should be answer");
    TEST_ASSERT(bridge.remote_candidates().size() == 1, "one remote candidate should be stored");
    return true;
}

bool test_webrtc_signaling_bridge_strict_sdp_validation_and_error_codes()
{
    WebrtcSignalingBridge bridge;

    SessionDescription invalid_empty;
    invalid_empty.type = SdpType::offer;
    invalid_empty.sdp = "";
    TEST_ASSERT(!bridge.set_remote_description(invalid_empty), "empty SDP should be rejected");
    TEST_ASSERT(bridge.last_sdp_error() == "empty_sdp", "empty SDP error code should match");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::new_, "state should remain new after empty SDP reject");

    SessionDescription invalid_parse;
    invalid_parse.type = SdpType::offer;
    invalid_parse.sdp = "v=1\r\n";
    TEST_ASSERT(!bridge.set_remote_description(invalid_parse), "invalid SDP parse should be rejected");
    TEST_ASSERT(bridge.last_sdp_error() == "sdp_parse_failed", "invalid SDP parse error code should match");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::new_, "state should remain new after parse reject");

    SessionDescription local_offer;
    local_offer.type = SdpType::offer;
    local_offer.sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    SessionDescription invalid_remote_offer;
    invalid_remote_offer.type = SdpType::offer;
    invalid_remote_offer.sdp = local_offer.sdp;

    TEST_ASSERT(bridge.set_local_description(local_offer), "valid local offer should pass");
    TEST_ASSERT(bridge.last_sdp_error().empty(), "error should clear after valid local offer");
    TEST_ASSERT(!bridge.set_remote_description(invalid_remote_offer), "invalid state transition should reject remote offer");
    TEST_ASSERT(bridge.last_sdp_error() == "invalid_state_transition", "state transition error code should match");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::have_local_offer, "state should stay have_local_offer after transition reject");

    SessionDescription remote_answer;
    remote_answer.type = SdpType::answer;
    remote_answer.sdp =
        "v=0\r\n"
        "o=- 2 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    TEST_ASSERT(bridge.set_remote_description(remote_answer), "valid remote answer should pass");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::stable, "state should move to stable after valid answer");
    TEST_ASSERT(bridge.has_parsed_remote_sdp(), "parsed remote SDP should be available after success");
    TEST_ASSERT(bridge.last_sdp_error().empty(), "error should clear after valid answer");
    return true;
}

bool test_webrtc_signaling_bridge_state_transitions()
{
    WebrtcSignalingBridge bridge;

    SessionDescription local_answer;
    local_answer.type = SdpType::answer;
    local_answer.sdp = valid_answer_sdp();

    SessionDescription local_offer;
    local_offer.type = SdpType::offer;
    local_offer.sdp = valid_offer_sdp();

    SessionDescription remote_offer;
    remote_offer.type = SdpType::offer;
    remote_offer.sdp = valid_offer_sdp();

    SessionDescription rollback;
    rollback.type = SdpType::rollback;
    rollback.sdp = valid_offer_sdp();

    TEST_ASSERT(!bridge.set_local_description(local_answer), "cannot set local answer before remote offer");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::new_, "state should remain new after invalid transition");

    TEST_ASSERT(bridge.set_local_description(local_offer), "local offer should be valid from new");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::have_local_offer, "state should be have_local_offer");

    TEST_ASSERT(!bridge.set_remote_description(remote_offer), "cannot set remote offer while have_local_offer");
    TEST_ASSERT(bridge.set_remote_description(rollback), "rollback should be valid from have_local_offer");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::stable, "rollback should move state to stable");

    TEST_ASSERT(bridge.set_remote_description(remote_offer), "remote offer should be valid from stable");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::have_remote_offer, "state should be have_remote_offer");
    TEST_ASSERT(bridge.set_local_description(local_answer), "local answer should be valid after remote offer");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::stable, "state should return to stable");

    return true;
}

bool test_webrtc_signaling_json_roundtrip_and_apply()
{
    WebrtcSignalingBridge bridge;
    const uint64_t duplicate_seen_before = bridge.diagnostics_v2_flat_duplicate_seen_count();
    const uint64_t duplicate_mismatch_before = bridge.diagnostics_v2_flat_duplicate_mismatch_count();

    WebrtcSignalingBridge::SignalingMessage offer_msg;
    offer_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::offer;
    offer_msg.description.type = SdpType::offer;
    offer_msg.description.sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    const std::string offer_json = bridge.to_signaling_json(offer_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_offer;
    TEST_ASSERT(bridge.parse_signaling_message(offer_json, parsed_offer), "offer JSON should parse");
    TEST_ASSERT(parsed_offer.kind == WebrtcSignalingBridge::SignalingMessage::Kind::offer, "parsed kind should be offer");
    TEST_ASSERT(parsed_offer.description.type == SdpType::offer, "parsed SDP type should be offer");
    TEST_ASSERT(parsed_offer.description.sdp == offer_msg.description.sdp, "parsed SDP should match");
    TEST_ASSERT(bridge.apply_signaling_message(parsed_offer, true), "remote offer should apply");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::have_remote_offer, "state should be have_remote_offer after apply");

    WebrtcSignalingBridge::SignalingMessage answer_msg;
    answer_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::answer;
    answer_msg.description.type = SdpType::answer;
    answer_msg.description.sdp =
        "v=0\r\n"
        "o=- 2 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    const std::string answer_json = bridge.to_signaling_json(answer_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_answer;
    TEST_ASSERT(bridge.parse_signaling_message(answer_json, parsed_answer), "answer JSON should parse");
    TEST_ASSERT(bridge.apply_signaling_message(parsed_answer, false), "local answer should apply");
    TEST_ASSERT(bridge.signaling_state() == SignalingState::stable, "state should be stable after answer apply");

    WebrtcSignalingBridge::SignalingMessage candidate_msg;
    candidate_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::candidate;
    candidate_msg.candidate.candidate = "candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host";
    candidate_msg.candidate.mid = "0";
    candidate_msg.candidate.mline_index = 0;

    const std::string candidate_json = bridge.to_signaling_json(candidate_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_candidate;
    TEST_ASSERT(bridge.parse_signaling_message(candidate_json, parsed_candidate), "candidate JSON should parse");
    TEST_ASSERT(bridge.apply_signaling_message(parsed_candidate, true), "candidate should apply as remote");
    TEST_ASSERT(bridge.remote_candidates().size() == 1, "candidate list should have one item");
    TEST_ASSERT(parsed_candidate.candidate.foundation == "1", "candidate foundation should parse from SDP text");
    TEST_ASSERT(parsed_candidate.candidate.transport == "udp", "candidate transport should parse and normalize");
    TEST_ASSERT(parsed_candidate.candidate.ip == "10.0.0.2", "candidate ip should parse");
    TEST_ASSERT(parsed_candidate.candidate.port == 5000, "candidate port should parse");

    WebrtcSignalingBridge::SignalingMessage structured_candidate;
    structured_candidate.kind = WebrtcSignalingBridge::SignalingMessage::Kind::candidate;
    structured_candidate.candidate.foundation = "99";
    structured_candidate.candidate.component = 1;
    structured_candidate.candidate.transport = "udp";
    structured_candidate.candidate.priority = 2122260223;
    structured_candidate.candidate.ip = "198.51.100.10";
    structured_candidate.candidate.port = 55000;
    structured_candidate.candidate.type = "host";
    structured_candidate.candidate.mid = "0";
    structured_candidate.candidate.mline_index = 0;
    const std::string structured_json = bridge.to_signaling_json(structured_candidate);
    WebrtcSignalingBridge::SignalingMessage parsed_structured_candidate;
    TEST_ASSERT(bridge.parse_signaling_message(structured_json, parsed_structured_candidate),
                "structured candidate JSON should parse");
    TEST_ASSERT(parsed_structured_candidate.candidate.foundation == "99",
                "structured candidate foundation should roundtrip");
    TEST_ASSERT(parsed_structured_candidate.candidate.ip == "198.51.100.10",
                "structured candidate ip should roundtrip");
    TEST_ASSERT(parsed_structured_candidate.candidate.port == 55000,
                "structured candidate port should roundtrip");

    WebrtcSignalingBridge::SignalingMessage invalid_msg;
    TEST_ASSERT(!bridge.parse_signaling_message("{\"type\":\"invalid\"}", invalid_msg), "invalid type should fail parsing");

    WebrtcSignalingBridge::SignalingMessage ice_msg;
    ice_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::ice_state;
    ice_msg.ice_transport_state = IceTransportState::checking;
    const std::string ice_json = bridge.to_signaling_json(ice_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_ice;
    TEST_ASSERT(bridge.parse_signaling_message(ice_json, parsed_ice), "ice_state JSON should parse");
    TEST_ASSERT(parsed_ice.kind == WebrtcSignalingBridge::SignalingMessage::Kind::ice_state, "parsed kind should be ice_state");
    TEST_ASSERT(parsed_ice.ice_transport_state == IceTransportState::checking, "parsed ice state should match");

    WebrtcSignalingBridge::SignalingMessage nomination_msg;
    nomination_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::ice_nomination;
    nomination_msg.ice_nomination_state = IceNominationState::nominated;
    nomination_msg.selected_pair_reason = IceSelectedPairReason::forced_by_signal;
    nomination_msg.selected_pair_reason_text = "controller_nomination";
    nomination_msg.nomination_transaction_id = "mock-stun-link-1";
    const std::string nomination_json = bridge.to_signaling_json(nomination_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_nomination;
    TEST_ASSERT(bridge.parse_signaling_message(nomination_json, parsed_nomination),
                "ice_nomination JSON should parse");
    TEST_ASSERT(parsed_nomination.kind == WebrtcSignalingBridge::SignalingMessage::Kind::ice_nomination,
                "parsed kind should be ice_nomination");
    TEST_ASSERT(parsed_nomination.ice_nomination_state == IceNominationState::nominated,
                "parsed ice nomination state should match");
    TEST_ASSERT(parsed_nomination.selected_pair_reason == IceSelectedPairReason::forced_by_signal,
                "parsed selected pair reason should match");
    TEST_ASSERT(parsed_nomination.selected_pair_reason_text == "controller_nomination",
                "parsed selected pair reason text should match");
    TEST_ASSERT(parsed_nomination.nomination_transaction_id == "mock-stun-link-1",
                "parsed nomination transaction id should match");

    WebrtcSignalingBridge::SignalingMessage diagnostics_msg;
    WebrtcSignalingBridge::DiagnosticsEmissionConfig compat_emit_cfg;
    compat_emit_cfg.emit_flat_compat_fields = true;
    compat_emit_cfg.release_mode_strict_v2 = false;
    bridge.set_diagnostics_emission_config(compat_emit_cfg);
    diagnostics_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::diagnostics;
    diagnostics_msg.diagnostics_scope = "peer_session";
    diagnostics_msg.diagnostics_schema_version = "v2";
    diagnostics_msg.diagnostics_ice_nomination_state = IceNominationState::nominated;
    diagnostics_msg.diagnostics_has_selected_pair = true;
    diagnostics_msg.diagnostics_selected_pair_reason = IceSelectedPairReason::forced_by_signal;
    diagnostics_msg.diagnostics_selected_pair_reason_text = "diagnostics_reason";
    diagnostics_msg.diagnostics_last_ice_error = "";
    diagnostics_msg.diagnostics_stun_transaction_count = 3;
    diagnostics_msg.diagnostics_has_last_stun_transaction = true;
    diagnostics_msg.diagnostics_last_stun_transaction_id = "tx-1";
    diagnostics_msg.diagnostics_last_stun_transaction_state = StunTransactionState::response_received;
    diagnostics_msg.diagnostics_pending_nomination_signal_count = 2;
    diagnostics_msg.diagnostics_dropped_nomination_signal_count = 5;
    diagnostics_msg.diagnostics_dropped_nomination_signal_overflow_count = 4;
    diagnostics_msg.diagnostics_dropped_nomination_signal_trim_count = 1;
    diagnostics_msg.diagnostics_pending_diagnostics_signal_count = 2;
    diagnostics_msg.diagnostics_dropped_diagnostics_signal_count = 7;
    diagnostics_msg.diagnostics_dropped_diagnostics_signal_overflow_count = 6;
    diagnostics_msg.diagnostics_dropped_diagnostics_signal_trim_count = 1;
    diagnostics_msg.diagnostics_policy_keep_latest_only = false;
    diagnostics_msg.diagnostics_policy_max_pending_signals = 11;
    diagnostics_msg.diagnostics_policy_nomination_max_pending_signals = 13;
    diagnostics_msg.diagnostics_rollout_alert_window_seconds = 86400;
    diagnostics_msg.diagnostics_rollout_mismatch_count_alert_threshold = 0;
    diagnostics_msg.diagnostics_rollout_mismatch_ratio_threshold_ppm = 1000;
    const std::string diagnostics_json = bridge.to_signaling_json(diagnostics_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_json, parsed_diagnostics),
                "diagnostics JSON should parse");
    TEST_ASSERT(parsed_diagnostics.kind == WebrtcSignalingBridge::SignalingMessage::Kind::diagnostics,
                "parsed diagnostics kind should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_scope == "peer_session",
                "parsed diagnostics scope should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_schema_version == "v2",
                "parsed diagnostics schema_version should match v2");
    TEST_ASSERT(parsed_diagnostics.diagnostics_ice_nomination_state == IceNominationState::nominated,
                "parsed diagnostics nomination state should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_selected_pair_reason == IceSelectedPairReason::forced_by_signal,
                "parsed diagnostics selected pair reason should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_last_stun_transaction_state == StunTransactionState::response_received,
                "parsed diagnostics last stun transaction state should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_nomination_signal_count == 5,
                "parsed diagnostics dropped counter should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_nomination_signal_overflow_count == 4,
                "parsed diagnostics dropped overflow counter should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_nomination_signal_trim_count == 1,
                "parsed diagnostics dropped trim counter should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_pending_diagnostics_signal_count == 2,
                "parsed diagnostics pending diagnostics signal count should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_diagnostics_signal_count == 7,
                "parsed diagnostics dropped diagnostics counter should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_diagnostics_signal_overflow_count == 6,
                "parsed diagnostics dropped diagnostics overflow counter should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_dropped_diagnostics_signal_trim_count == 1,
                "parsed diagnostics dropped diagnostics trim counter should match");
    TEST_ASSERT(!parsed_diagnostics.diagnostics_policy_keep_latest_only,
                "parsed diagnostics policy keep_latest_only should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_policy_max_pending_signals == 11,
                "parsed diagnostics policy max_pending_signals should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_policy_nomination_max_pending_signals == 13,
                "parsed diagnostics policy nomination max_pending_signals should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_rollout_alert_window_seconds == 86400,
                "parsed diagnostics rollout alert window should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_rollout_mismatch_count_alert_threshold == 0,
                "parsed diagnostics rollout mismatch count threshold should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_rollout_mismatch_ratio_threshold_ppm == 1000,
                "parsed diagnostics rollout mismatch ratio threshold should match");
    TEST_ASSERT(parsed_diagnostics.diagnostics_rollout_current_mismatch_ratio_ppm == 0,
                "parsed diagnostics rollout current mismatch ratio should default to zero");
    TEST_ASSERT(!parsed_diagnostics.diagnostics_rollout_alert_active,
                "parsed diagnostics rollout alert should default to false");
    TEST_ASSERT(!parsed_diagnostics.diagnostics_rollout_progress_blocked,
                "parsed diagnostics rollout progress blocked should default to false");
    TEST_ASSERT(parsed_diagnostics.diagnostics_rollout_ready_for_progress,
                "parsed diagnostics rollout ready_for_progress should default to true");
    TEST_ASSERT(bridge.diagnostics_v2_flat_duplicate_seen_count() > duplicate_seen_before,
                "diagnostics duplicate telemetry: seen counter should increase after parsing nested+flat diagnostics");

    const std::string diagnostics_nested_policy_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"ice_nomination_state\":\"nominated\","
        "\"has_selected_pair\":true,\"selected_pair_reason\":\"forced_by_signal\","
        "\"selected_pair_reason_text\":\"diagnostics_reason\",\"last_ice_error\":\"\","
        "\"stun_transaction_count\":3,\"has_last_stun_transaction\":true,"
        "\"last_stun_transaction_id\":\"tx-1\",\"last_stun_transaction_state\":\"response_received\","
        "\"pending_nomination_signal_count\":2,\"dropped_nomination_signal_count\":5,"
        "\"dropped_nomination_signal_overflow_count\":4,\"dropped_nomination_signal_trim_count\":1,"
        "\"pending_diagnostics_signal_count\":2,\"dropped_diagnostics_signal_count\":7,"
        "\"dropped_diagnostics_signal_overflow_count\":6,\"dropped_diagnostics_signal_trim_count\":1,"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics_nested;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_nested_policy_json, parsed_diagnostics_nested),
                "diagnostics JSON with nested policy object should parse");
    TEST_ASSERT(!parsed_diagnostics_nested.diagnostics_policy_keep_latest_only,
                "nested policy parse keep_latest_only should match");
    TEST_ASSERT(parsed_diagnostics_nested.diagnostics_policy_max_pending_signals == 11,
                "nested policy parse max_pending_signals should match");
    TEST_ASSERT(parsed_diagnostics_nested.diagnostics_policy_nomination_max_pending_signals == 13,
                "nested policy parse nomination max_pending_signals should match");
    TEST_ASSERT(parsed_diagnostics_nested.diagnostics_schema_version == "v2",
                "nested policy parse should infer schema_version v2");

    const std::string diagnostics_nested_queues_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"ice_nomination_state\":\"nominated\","
        "\"has_selected_pair\":true,\"selected_pair_reason\":\"forced_by_signal\","
        "\"selected_pair_reason_text\":\"diagnostics_reason\",\"last_ice_error\":\"\","
        "\"stun_transaction_count\":3,\"has_last_stun_transaction\":true,"
        "\"last_stun_transaction_id\":\"tx-1\",\"last_stun_transaction_state\":\"response_received\","
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics_nested_queues;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_nested_queues_json, parsed_diagnostics_nested_queues),
                "diagnostics JSON with nested queues object should parse");
    TEST_ASSERT(parsed_diagnostics_nested_queues.diagnostics_pending_nomination_signal_count == 2,
                "nested queues parse pending nomination count should match");
    TEST_ASSERT(parsed_diagnostics_nested_queues.diagnostics_dropped_nomination_signal_count == 5,
                "nested queues parse dropped nomination count should match");
    TEST_ASSERT(parsed_diagnostics_nested_queues.diagnostics_pending_diagnostics_signal_count == 2,
                "nested queues parse pending diagnostics count should match");
    TEST_ASSERT(parsed_diagnostics_nested_queues.diagnostics_dropped_diagnostics_signal_count == 7,
                "nested queues parse dropped diagnostics count should match");
    TEST_ASSERT(parsed_diagnostics_nested_queues.diagnostics_schema_version == "v2",
                "nested queues parse should infer schema_version v2");

    const std::string diagnostics_nested_nomination_stun_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\","
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,"
        "\"has_last_transaction\":true,\"last_transaction_id\":\"tx-1\","
        "\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics_nested_v2;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_nested_nomination_stun_json, parsed_diagnostics_nested_v2),
                "diagnostics JSON with nested nomination/stun objects should parse");
    TEST_ASSERT(parsed_diagnostics_nested_v2.diagnostics_ice_nomination_state == IceNominationState::nominated,
                "nested nomination parse state should match");
    TEST_ASSERT(parsed_diagnostics_nested_v2.diagnostics_stun_transaction_count == 3,
                "nested stun parse transaction count should match");
    TEST_ASSERT(parsed_diagnostics_nested_v2.diagnostics_has_last_stun_transaction,
                "nested stun parse has_last_transaction should match");
    TEST_ASSERT(parsed_diagnostics_nested_v2.diagnostics_schema_version == "v2",
                "nested nomination/stun parse should infer schema_version v2");

    const std::string diagnostics_nested_migration_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,"
        "\"has_last_transaction\":true,\"last_transaction_id\":\"tx-1\","
        "\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13},"
        "\"rollout_policy\":{\"alert_window_seconds\":86400,"
        "\"mismatch_count_alert_threshold\":0,\"mismatch_ratio_threshold_ppm\":1000},"
        "\"migration\":{\"v2_flat_duplicate_seen_count\":9,\"v2_flat_duplicate_mismatch_count\":2}}";
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics_nested_migration;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_nested_migration_json, parsed_diagnostics_nested_migration),
                "diagnostics JSON with nested migration object should parse");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_v2_flat_duplicate_seen_count == 9,
                "nested migration parse seen counter should match");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_v2_flat_duplicate_mismatch_count == 2,
                "nested migration parse mismatch counter should match");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_rollout_alert_window_seconds == 86400,
                "nested rollout policy parse alert window should match");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_rollout_mismatch_count_alert_threshold == 0,
                "nested rollout policy parse mismatch count threshold should match");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_rollout_mismatch_ratio_threshold_ppm == 1000,
                "nested rollout policy parse mismatch ratio threshold should match");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_rollout_current_mismatch_ratio_ppm == 0,
                "nested rollout policy parse current mismatch ratio should default to zero");
    TEST_ASSERT(!parsed_diagnostics_nested_migration.diagnostics_rollout_alert_active,
                "nested rollout policy parse alert should default to false");
    TEST_ASSERT(!parsed_diagnostics_nested_migration.diagnostics_rollout_progress_blocked,
                "nested rollout policy parse progress blocked should default to false");
    TEST_ASSERT(parsed_diagnostics_nested_migration.diagnostics_rollout_ready_for_progress,
                "nested rollout policy parse ready_for_progress should default to true");

    const std::string diagnostics_flat_v1_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"ice_nomination_state\":\"nominated\","
        "\"has_selected_pair\":true,\"selected_pair_reason\":\"forced_by_signal\","
        "\"selected_pair_reason_text\":\"diagnostics_reason\",\"last_ice_error\":\"\","
        "\"stun_transaction_count\":3,\"has_last_stun_transaction\":true,"
        "\"last_stun_transaction_id\":\"tx-1\",\"last_stun_transaction_state\":\"response_received\","
        "\"pending_nomination_signal_count\":2,\"dropped_nomination_signal_count\":5,"
        "\"dropped_nomination_signal_overflow_count\":4,\"dropped_nomination_signal_trim_count\":1,"
        "\"pending_diagnostics_signal_count\":2,\"dropped_diagnostics_signal_count\":7,"
        "\"dropped_diagnostics_signal_overflow_count\":6,\"dropped_diagnostics_signal_trim_count\":1,"
        "\"policy_keep_latest_only\":false,\"policy_max_pending_signals\":11,"
        "\"policy_nomination_max_pending_signals\":13}";
    WebrtcSignalingBridge::SignalingMessage parsed_diagnostics_flat_v1;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_flat_v1_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON with flat v1-like fields should parse");
    TEST_ASSERT(parsed_diagnostics_flat_v1.diagnostics_schema_version == "v1",
                "flat-only diagnostics parse should infer schema_version v1");

    const std::string diagnostics_explicit_v2_missing_nested_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
        "\"ice_nomination_state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\","
        "\"last_ice_error\":\"\",\"stun_transaction_count\":3,\"has_last_stun_transaction\":true,"
        "\"last_stun_transaction_id\":\"tx-1\",\"last_stun_transaction_state\":\"response_received\","
        "\"pending_nomination_signal_count\":2,\"dropped_nomination_signal_count\":5,"
        "\"dropped_nomination_signal_overflow_count\":4,\"dropped_nomination_signal_trim_count\":1,"
        "\"pending_diagnostics_signal_count\":2,\"dropped_diagnostics_signal_count\":7,"
        "\"dropped_diagnostics_signal_overflow_count\":6,\"dropped_diagnostics_signal_trim_count\":1,"
        "\"policy_keep_latest_only\":false,\"policy_max_pending_signals\":11,"
        "\"policy_nomination_max_pending_signals\":13}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_explicit_v2_missing_nested_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON with schema_version v2 should reject when nested v2 objects are missing");

    const std::string diagnostics_explicit_v2_mismatched_duplicate_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13},"
        "\"ice_nomination_state\":\"failed\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\","
        "\"last_ice_error\":\"\",\"stun_transaction_count\":3,\"has_last_stun_transaction\":true,"
        "\"last_stun_transaction_id\":\"tx-1\",\"last_stun_transaction_state\":\"response_received\","
        "\"pending_nomination_signal_count\":2,\"dropped_nomination_signal_count\":5,"
        "\"dropped_nomination_signal_overflow_count\":4,\"dropped_nomination_signal_trim_count\":1,"
        "\"pending_diagnostics_signal_count\":2,\"dropped_diagnostics_signal_count\":7,"
        "\"dropped_diagnostics_signal_overflow_count\":6,\"dropped_diagnostics_signal_trim_count\":1,"
        "\"policy_keep_latest_only\":false,\"policy_max_pending_signals\":11,"
        "\"policy_nomination_max_pending_signals\":13}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_explicit_v2_mismatched_duplicate_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON with schema_version v2 should reject mismatched nested/flat duplicate values");
    TEST_ASSERT(bridge.diagnostics_v2_flat_duplicate_mismatch_count() > duplicate_mismatch_before,
                "diagnostics duplicate telemetry: mismatch counter should increase on duplicate mismatch reject");

    const std::string diagnostics_explicit_v1_nested_only_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v1\","
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_explicit_v1_nested_only_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON with schema_version v1 should reject nested-only payload without flat fields");

    const std::string diagnostics_emit_false_v1_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v1\","
        "\"emit_flat_compat_fields\":false,"
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13},"
        "\"migration\":{\"v2_flat_duplicate_seen_count\":9,\"v2_flat_duplicate_mismatch_count\":2}}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_emit_false_v1_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON should reject emit_flat_compat_fields=false combined with schema_version v1");

    const std::string diagnostics_release_mode_v1_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v1\","
        "\"emit_flat_compat_fields\":false,\"release_mode_strict_v2\":true,"
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13},"
        "\"migration\":{\"v2_flat_duplicate_seen_count\":9,\"v2_flat_duplicate_mismatch_count\":2}}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_release_mode_v1_json, parsed_diagnostics_flat_v1),
                "diagnostics JSON should reject release_mode_strict_v2=true with schema_version v1");

    WebrtcSignalingBridge::DiagnosticsEmissionConfig strict_cfg;
    strict_cfg.emit_flat_compat_fields = true;
    strict_cfg.release_mode_strict_v2 = true;
    bridge.set_diagnostics_emission_config(strict_cfg);

    WebrtcSignalingBridge::SignalingMessage strict_emit_msg = diagnostics_msg;
    strict_emit_msg.diagnostics_schema_version = "v1";
    strict_emit_msg.diagnostics_emit_flat_compat_fields = true;
    strict_emit_msg.diagnostics_release_mode_strict_v2 = false;
    const std::string strict_emit_json = bridge.to_signaling_json(strict_emit_msg);
    TEST_ASSERT(strict_emit_json.find("\"schema_version\":\"v2\"") != std::string::npos,
                "diagnostics strict release mode should force schema_version v2 on emission");
    TEST_ASSERT(strict_emit_json.find("\"emit_flat_compat_fields\":false") != std::string::npos,
                "diagnostics strict release mode should force nested-only emission");
    TEST_ASSERT(strict_emit_json.find("\"release_mode_strict_v2\":true") != std::string::npos,
                "diagnostics strict release mode should emit release_mode_strict_v2=true");
    TEST_ASSERT(strict_emit_json.find("\"ice_nomination_state\":") == std::string::npos,
                "diagnostics strict release mode should omit flat compatibility fields");

    WebrtcSignalingBridge::SignalingMessage parsed_strict_emit;
    TEST_ASSERT(bridge.parse_signaling_message(strict_emit_json, parsed_strict_emit),
                "diagnostics strict release-mode payload should parse");
    TEST_ASSERT(parsed_strict_emit.diagnostics_release_mode_strict_v2,
                "diagnostics strict release-mode parse should set release_mode_strict_v2=true");
    TEST_ASSERT(!parsed_strict_emit.diagnostics_emit_flat_compat_fields,
                "diagnostics strict release-mode parse should set emit_flat_compat_fields=false");
    TEST_ASSERT(parsed_strict_emit.diagnostics_schema_version == "v2",
                "diagnostics strict release-mode parse should set schema_version v2");

    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_flat_v1_json, parsed_diagnostics_flat_v1),
                "diagnostics parser should reject flat-only payload while strict release mode is enabled");

    const std::string diagnostics_strict_override_attempt_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
        "\"emit_flat_compat_fields\":true,\"release_mode_strict_v2\":false,"
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    TEST_ASSERT(!bridge.parse_signaling_message(diagnostics_strict_override_attempt_json, parsed_diagnostics_flat_v1),
                "diagnostics parser should reject attempts to disable strict mode in payload");

    const std::string diagnostics_strict_minimal_v2_json =
        "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
        "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
        "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diagnostics_reason\"},"
        "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":3,\"has_last_transaction\":true,"
        "\"last_transaction_id\":\"tx-1\",\"last_transaction_state\":\"response_received\"},"
        "\"queues\":{\"nomination\":{\"pending_signal_count\":2,\"dropped_signal_count\":5,"
        "\"dropped_signal_overflow_count\":4,\"dropped_signal_trim_count\":1},"
        "\"diagnostics\":{\"pending_signal_count\":2,\"dropped_signal_count\":7,"
        "\"dropped_signal_overflow_count\":6,\"dropped_signal_trim_count\":1}},"
        "\"policy\":{\"keep_latest_only\":false,\"max_pending_signals\":11,"
        "\"nomination_max_pending_signals\":13}}";
    WebrtcSignalingBridge::SignalingMessage parsed_strict_minimal_v2;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_strict_minimal_v2_json, parsed_strict_minimal_v2),
                "diagnostics parser should accept nested v2 payload under strict mode defaults");
    TEST_ASSERT(parsed_strict_minimal_v2.diagnostics_release_mode_strict_v2,
                "diagnostics parser strict mode should set release_mode_strict_v2=true when configured");
    TEST_ASSERT(!parsed_strict_minimal_v2.diagnostics_emit_flat_compat_fields,
                "diagnostics parser strict mode should default emit_flat_compat_fields=false");

    WebrtcSignalingBridge::DiagnosticsEmissionConfig default_cfg;
    bridge.set_diagnostics_emission_config(default_cfg);

    WebrtcSignalingBridge::SignalingMessage parsed_non_strict_override;
    TEST_ASSERT(bridge.parse_signaling_message(diagnostics_strict_override_attempt_json, parsed_non_strict_override),
                "diagnostics parser should allow release_mode_strict_v2=false payload when runtime strict mode is disabled");
    TEST_ASSERT(!parsed_non_strict_override.diagnostics_release_mode_strict_v2,
                "diagnostics parser should preserve release_mode_strict_v2=false in non-strict runtime mode");
    TEST_ASSERT(parsed_non_strict_override.diagnostics_emit_flat_compat_fields,
                "diagnostics parser should preserve emit_flat_compat_fields=true in non-strict runtime mode");

    WebrtcSignalingBridge::SignalingMessage default_emit_msg = diagnostics_msg;
    default_emit_msg.diagnostics_schema_version = "v2";
    const std::string default_emit_json = bridge.to_signaling_json(default_emit_msg);
    TEST_ASSERT(default_emit_json.find("\"emit_flat_compat_fields\":false") != std::string::npos,
                "diagnostics default emission should disable flat compatibility fields");
    TEST_ASSERT(default_emit_json.find("\"ice_nomination_state\":") == std::string::npos,
                "diagnostics default emission should omit flat nomination compatibility fields");
    TEST_ASSERT(default_emit_json.find("\"schema_version\":\"v2\"") != std::string::npos,
                "diagnostics default emission should keep schema_version v2");

    WebrtcSignalingBridge::SignalingMessage dtls_msg;
    dtls_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::dtls_state;
    dtls_msg.dtls_transport_state = DtlsTransportState::connecting;
    const std::string dtls_json = bridge.to_signaling_json(dtls_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_dtls;
    TEST_ASSERT(bridge.parse_signaling_message(dtls_json, parsed_dtls), "dtls_state JSON should parse");
    TEST_ASSERT(parsed_dtls.kind == WebrtcSignalingBridge::SignalingMessage::Kind::dtls_state, "parsed kind should be dtls_state");
    TEST_ASSERT(parsed_dtls.dtls_transport_state == DtlsTransportState::connecting, "parsed dtls state should match");

    WebrtcSignalingBridge::SignalingMessage sec_msg;
    sec_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::security_state;
    sec_msg.security_error = "dtls_fingerprint_mismatch";
    const std::string sec_json = bridge.to_signaling_json(sec_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_sec;
    TEST_ASSERT(bridge.parse_signaling_message(sec_json, parsed_sec), "security_state JSON should parse");
    TEST_ASSERT(parsed_sec.kind == WebrtcSignalingBridge::SignalingMessage::Kind::security_state,
                "parsed kind should be security_state");
    TEST_ASSERT(parsed_sec.security_error == "dtls_fingerprint_mismatch",
                "parsed security error should match");
    TEST_ASSERT(parsed_sec.security_error_code == SecurityErrorCode::dtls_fingerprint_mismatch,
                "parsed security code should match canonical fingerprint mismatch");

    WebrtcSignalingBridge::SignalingMessage sec_ext_msg;
    sec_ext_msg.kind = WebrtcSignalingBridge::SignalingMessage::Kind::security_state;
    sec_ext_msg.security_error_code = SecurityErrorCode::external_security_error;
    sec_ext_msg.security_error = "policy_blocked_by_control_plane";
    const std::string sec_ext_json = bridge.to_signaling_json(sec_ext_msg);
    WebrtcSignalingBridge::SignalingMessage parsed_sec_ext;
    TEST_ASSERT(bridge.parse_signaling_message(sec_ext_json, parsed_sec_ext),
                "external security_state JSON should parse");
    TEST_ASSERT(parsed_sec_ext.security_error_code == SecurityErrorCode::external_security_error,
                "external security_state code should remain external");
    TEST_ASSERT(parsed_sec_ext.security_error == "policy_blocked_by_control_plane",
                "external security_state should keep custom message");

    WebrtcSignalingBridge::SignalingMessage parsed_code_priority;
    TEST_ASSERT(bridge.parse_signaling_message(
                    "{\"type\":\"security_state\",\"security_code\":\"dtls_fingerprint_mismatch\",\"security_error\":\"ignored_legacy_text\"}",
                    parsed_code_priority),
                "security_state with both fields should parse");
    TEST_ASSERT(parsed_code_priority.security_error_code == SecurityErrorCode::dtls_fingerprint_mismatch,
                "security_code should be parsed");
    TEST_ASSERT(parsed_code_priority.security_error == "dtls_fingerprint_mismatch",
                "security_code should take precedence over legacy string");

    WebrtcSignalingBridge::SignalingMessage parsed_legacy_only;
    TEST_ASSERT(bridge.parse_signaling_message(
                    "{\"type\":\"security_state\",\"security_error\":\"legacy_unknown_reason\"}",
                    parsed_legacy_only),
                "legacy security_state should parse without security_code");
    TEST_ASSERT(parsed_legacy_only.security_error_code == SecurityErrorCode::external_security_error,
                "legacy unknown reason should map to external code");
    TEST_ASSERT(parsed_legacy_only.security_error == "legacy_unknown_reason",
                "legacy unknown reason should be preserved");

    TEST_ASSERT(!bridge.parse_signaling_message(
                     "{\"type\":\"security_state\",\"security_code\":\"unknown_code\"}",
                     invalid_msg),
                "unknown security_code should be rejected");
    return true;
}

bool test_webrtc_signaling_bridge_negotiation_validation_errors()
{
    WebrtcSignalingBridge bridge;

    SessionDescription local_offer;
    local_offer.type = SdpType::offer;
    local_offer.sdp = offer_two_media_sdp();
    TEST_ASSERT(bridge.set_local_description(local_offer), "negotiation: local offer should apply");

    SessionDescription remote_bad_count;
    remote_bad_count.type = SdpType::answer;
    remote_bad_count.sdp = valid_answer_sdp();
    TEST_ASSERT(!bridge.set_remote_description(remote_bad_count), "negotiation: answer with fewer media should fail");
    TEST_ASSERT(bridge.last_sdp_error() == "sdp_media_count_mismatch",
                "negotiation: media count mismatch error should match");

    SessionDescription remote_bad_kind;
    remote_bad_kind.type = SdpType::answer;
    remote_bad_kind.sdp =
        "v=0\r\n"
        "o=- 11 11 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 VP8/90000\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    TEST_ASSERT(!bridge.set_remote_description(remote_bad_kind), "negotiation: answer with media kind mismatch should fail");
    TEST_ASSERT(bridge.last_sdp_error() == "sdp_media_kind_mismatch",
                "negotiation: media kind mismatch error should match");

    SessionDescription remote_bad_mid;
    remote_bad_mid.type = SdpType::answer;
    remote_bad_mid.sdp =
        "v=0\r\n"
        "o=- 12 12 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:x\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    TEST_ASSERT(!bridge.set_remote_description(remote_bad_mid), "negotiation: answer with mid mismatch should fail");
    TEST_ASSERT(bridge.last_sdp_error() == "sdp_mid_mismatch",
                "negotiation: mid mismatch error should match");

    SessionDescription remote_bad_payload;
    remote_bad_payload.type = SdpType::answer;
    remote_bad_payload.sdp =
        "v=0\r\n"
        "o=- 13 13 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 112\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:112 opus/48000/2\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 97\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:97 VP9/90000\r\n";
    TEST_ASSERT(!bridge.set_remote_description(remote_bad_payload), "negotiation: answer with payload mismatch should fail");
    TEST_ASSERT(bridge.last_sdp_error() == "sdp_payload_mismatch",
                "negotiation: payload mismatch error should match");

    SessionDescription remote_good_answer;
    remote_good_answer.type = SdpType::answer;
    remote_good_answer.sdp =
        "v=0\r\n"
        "o=- 14 14 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
        "a=mid:1\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:96 VP8/90000\r\n";
    TEST_ASSERT(bridge.set_remote_description(remote_good_answer), "negotiation: matching answer should pass");
    TEST_ASSERT(bridge.last_sdp_error().empty(), "negotiation: error should clear after valid answer");

    SessionDescription local_offer_for_codec_check;
    local_offer_for_codec_check.type = SdpType::offer;
    local_offer_for_codec_check.sdp =
        "v=0\r\n"
        "o=- 20 20 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    WebrtcSignalingBridge codec_bridge;
    TEST_ASSERT(codec_bridge.set_local_description(local_offer_for_codec_check),
                "negotiation: codec check local offer should apply");

    SessionDescription remote_bad_codec_map;
    remote_bad_codec_map.type = SdpType::answer;
    remote_bad_codec_map.sdp =
        "v=0\r\n"
        "o=- 21 21 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=mid:0\r\n"
        "a=sendrecv\r\n"
        "a=rtpmap:111 PCMU/8000\r\n";
    TEST_ASSERT(!codec_bridge.set_remote_description(remote_bad_codec_map),
                "negotiation: payload PT with incompatible rtpmap should fail");
    TEST_ASSERT(codec_bridge.last_sdp_error() == "sdp_payload_mismatch",
                "negotiation: incompatible rtpmap should report payload mismatch");

    return true;
}

bool test_webrtc_candidate_parser_and_ice_scaffold_lifecycle()
{
    IceCandidate parsed;
    const std::string candidate_text =
        "candidate:842163049 1 UDP 1686052607 203.0.113.1 54400 typ srflx raddr 10.0.0.2 rport 54400";
    TEST_ASSERT(parse_ice_candidate_sdp(candidate_text, parsed), "candidate parser should parse srflx candidate");
    TEST_ASSERT(parsed.foundation == "842163049", "candidate parser should parse foundation");
    TEST_ASSERT(parsed.component == 1, "candidate parser should parse component");
    TEST_ASSERT(parsed.transport == "udp", "candidate parser should normalize transport");
    TEST_ASSERT(parsed.priority == 1686052607u, "candidate parser should parse priority");
    TEST_ASSERT(parsed.ip == "203.0.113.1", "candidate parser should parse ip");
    TEST_ASSERT(parsed.port == 54400, "candidate parser should parse port");
    TEST_ASSERT(parsed.type == "srflx", "candidate parser should parse type");
    TEST_ASSERT(parsed.related_address == "10.0.0.2", "candidate parser should parse raddr");
    TEST_ASSERT(parsed.related_port == 54400, "candidate parser should parse rport");

    WebrtcPeerSession peer(0xBADA55EE, 90000);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::new_, "ice scaffold: initial gathering state should be new");
    TEST_ASSERT(peer.ice_checklist_state() == IceChecklistState::idle, "ice scaffold: initial checklist state should be idle");

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::gathering,
                "ice scaffold: state should move to gathering after start");

    peer.advance_transport(130);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::complete,
                "ice scaffold: gathering should complete after adapter delay");
    TEST_ASSERT(!peer.local_ice_candidates().empty(),
                "ice scaffold: local candidates should be produced by scaffold adapter");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    140),
                "ice scaffold: remote candidate should apply");
    TEST_ASSERT(peer.ice_checklist_state() == IceChecklistState::running,
                "ice scaffold: checklist should be running after remote candidate");

    peer.advance_transport(220);
    TEST_ASSERT(peer.ice_checklist_state() == IceChecklistState::completed,
                "ice scaffold: checklist should complete after delay");

    auto snap = peer.snapshot();
    TEST_ASSERT(snap.ice_gathering_state == IceGatheringState::complete,
                "ice scaffold: snapshot gathering state should be complete");
    TEST_ASSERT(snap.ice_checklist_state == IceChecklistState::completed,
                "ice scaffold: snapshot checklist state should be completed");
    TEST_ASSERT(snap.ice_nomination_state == IceNominationState::nominated,
                "ice scaffold: snapshot nomination should be nominated");
    TEST_ASSERT(snap.has_selected_ice_pair,
                "ice scaffold: snapshot should report selected pair");
    TEST_ASSERT(snap.selected_ice_pair_reason == IceSelectedPairReason::nominated_by_provider,
                "ice scaffold: selected pair reason should come from provider/scaffold nomination");
    TEST_ASSERT(snap.selected_ice_pair_reason_text == "scaffold_nominated"
                    || snap.selected_ice_pair_reason_text == "provider_nominated",
                "ice scaffold: selected pair reason text should be scaffold/provider nomination text");
    TEST_ASSERT(peer.last_ice_error().empty(), "ice scaffold: last ICE error should be empty when nomination succeeds");
    return true;
}

bool test_webrtc_peer_session_provider_backed_ice_failure_path()
{
    WebrtcPeerSession peer(0xCCCCCC01, 90000);
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = false;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 20;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    peer.advance_transport(120);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::complete,
                "provider ice failure: gathering should complete");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:9 1 UDP 2122260223 10.0.0.9 5001 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    130),
                "provider ice failure: remote candidate should apply");

    peer.advance_transport(150);
    peer.advance_transport(220);

    TEST_ASSERT(peer.ice_checklist_state() == IceChecklistState::completed,
                "provider ice failure: checklist should complete with remote candidate");
    TEST_ASSERT(peer.ice_nomination_state() == IceNominationState::nominated,
                "provider ice failure: nomination should be marked nominated by provider");
    TEST_ASSERT(peer.connection_state() != PeerConnectionState::failed,
                "provider ice failure: connection should not hard-fail when provider nominates");
    TEST_ASSERT(peer.last_ice_error().empty(),
                "provider ice failure: last ICE error should be empty after successful nomination");
    const auto snap = peer.snapshot();
    TEST_ASSERT(!snap.stun_transactions.empty(),
                "provider ice failure: snapshot should expose STUN transactions");
    TEST_ASSERT(snap.has_last_stun_transaction,
                "provider ice failure: snapshot should expose last STUN transaction presence");
    TEST_ASSERT(snap.last_stun_transaction.state == StunTransactionState::response_received,
                "provider ice failure: last STUN transaction should complete with response");
    TEST_ASSERT(snap.last_stun_transaction.request_count >= 1,
                "provider ice failure: STUN transaction should include request count");
    TEST_ASSERT(snap.last_stun_transaction.response_code == "success",
                "provider ice failure: STUN response code should be success");
    TEST_ASSERT(!snap.last_stun_transaction.mapped_address.empty(),
                "provider ice failure: STUN response should carry mapped address");
    return true;
}

bool test_webrtc_peer_session_provider_stun_timeout_and_nomination_signal()
{
    WebrtcPeerSession peer(0xCCCCCC02, 90000);
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 40;
    cfg.stun_response_delay_ms = 25;
    cfg.stun_timeout_ms = 15;
    cfg.stun_retry_interval_ms = 5;
    cfg.stun_max_retransmits = 3;
    cfg.force_stun_timeout = true;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    peer.advance_transport(120);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::complete,
                "provider stun timeout: gathering should complete");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:7 1 UDP 2122260223 10.0.0.7 5007 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    130),
                "provider stun timeout: remote candidate should apply");

    peer.advance_transport(200);

    TEST_ASSERT(peer.ice_checklist_state() == IceChecklistState::failed,
                "provider stun timeout: checklist should fail on timeout");
    TEST_ASSERT(peer.ice_nomination_state() == IceNominationState::failed,
                "provider stun timeout: nomination should fail on timeout");
    TEST_ASSERT(peer.last_ice_error() == "provider_stun_timeout",
                "provider stun timeout: error should report STUN timeout");

    auto snap = peer.snapshot();
    TEST_ASSERT(!snap.stun_transactions.empty(),
                "provider stun timeout: snapshot should contain STUN transaction history");
    TEST_ASSERT(snap.has_last_stun_transaction,
                "provider stun timeout: snapshot should contain last STUN transaction");
    TEST_ASSERT(snap.last_stun_transaction.state == StunTransactionState::timed_out,
                "provider stun timeout: last STUN transaction should be timed out");
    TEST_ASSERT(snap.last_stun_transaction.error == "stun_timeout",
                "provider stun timeout: timed out transaction should carry timeout error");
    TEST_ASSERT(snap.last_stun_transaction.response_code == "timeout",
                "provider stun timeout: timed out transaction should carry timeout response code");
    TEST_ASSERT(snap.last_stun_transaction.retransmit_count > 0,
                "provider stun timeout: timed out transaction should include retransmits");
    TEST_ASSERT(snap.last_stun_transaction.request_count == snap.last_stun_transaction.retransmit_count + 1,
                "provider stun timeout: request count should equal initial request plus retransmits");

    const std::string timeout_tx_id = snap.last_stun_transaction.transaction_id;
    TEST_ASSERT(peer.apply_signaling_json(
                    std::string("{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"controller_override\",\"nomination_transaction_id\":\"")
                        + timeout_tx_id + "\"}",
                    true,
                    260),
                "provider stun timeout: forced nomination signaling should apply");

    TEST_ASSERT(peer.ice_nomination_state() == IceNominationState::nominated,
                "provider stun timeout: nomination should be overridden by signal");
    TEST_ASSERT(peer.has_selected_ice_pair(),
                "provider stun timeout: forced nomination should provide selected pair");
    const IceCandidatePair forced_pair = peer.selected_ice_pair();
    TEST_ASSERT(forced_pair.reason == IceSelectedPairReason::forced_by_signal,
                "provider stun timeout: selected pair reason should indicate forced signal");
    TEST_ASSERT(forced_pair.reason_text == "controller_override",
                "provider stun timeout: selected pair reason text should preserve signal text");
    TEST_ASSERT(forced_pair.nomination_transaction_id == timeout_tx_id,
                "provider stun timeout: selected pair should keep nomination transaction id");
    return true;
}

bool test_webrtc_peer_session_provider_parallel_stun_transactions_and_snapshot_roundtrip()
{
    WebrtcPeerSession peer(0xCCCCCC03, 90000);
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 70;
    cfg.stun_response_delay_ms = 60;
    cfg.stun_timeout_ms = 20;
    cfg.stun_retry_interval_ms = 6;
    cfg.stun_max_retransmits = 4;
    cfg.force_stun_timeout = false;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    peer.advance_transport(120);
    TEST_ASSERT(peer.ice_gathering_state() == IceGatheringState::complete,
                "provider parallel stun: gathering should complete");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:11 1 UDP 2122260223 10.0.0.11 5011 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    130),
                "provider parallel stun: first remote candidate should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:12 1 UDP 2122260223 10.0.0.12 5012 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    136),
                "provider parallel stun: second remote candidate should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:13 1 UDP 2122260223 10.0.0.13 5013 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    142),
                "provider parallel stun: third remote candidate should apply");

    peer.advance_transport(230);
    const auto snap = peer.snapshot();
    TEST_ASSERT(snap.stun_transactions.size() >= 3,
                "provider parallel stun: should track multiple STUN transactions");
    TEST_ASSERT(snap.has_last_stun_transaction,
                "provider parallel stun: should expose last STUN transaction");
    TEST_ASSERT(snap.last_stun_transaction.request_count >= 2,
                "provider parallel stun: last transaction should show retries before response");
    TEST_ASSERT(snap.last_stun_transaction.response_code == "success"
                    || snap.last_stun_transaction.response_code == "",
                "provider parallel stun: last transaction should end in success");
    TEST_ASSERT(snap.last_stun_transaction.state == StunTransactionState::response_received,
                "provider parallel stun: last transaction should be response_received");

    const std::string snap_json = peer.snapshot_json();
    WebrtcPeerSessionSnapshot parsed;
    TEST_ASSERT(peer.parse_snapshot_json(snap_json, parsed),
                "provider parallel stun: snapshot JSON should parse");
    TEST_ASSERT(parsed.stun_transactions.size() == snap.stun_transactions.size(),
                "provider parallel stun: parsed STUN transaction count should match");
    TEST_ASSERT(parsed.last_stun_transaction.request_count == snap.last_stun_transaction.request_count,
                "provider parallel stun: parsed request count should match");
    TEST_ASSERT(parsed.last_stun_transaction.retransmit_count == snap.last_stun_transaction.retransmit_count,
                "provider parallel stun: parsed retransmit count should match");
    TEST_ASSERT(parsed.last_stun_transaction.response_code == snap.last_stun_transaction.response_code,
                "provider parallel stun: parsed response code should match");

    const std::string linked_tx_id = snap.last_stun_transaction.transaction_id;
    TEST_ASSERT(peer.apply_signaling_json(
                    std::string("{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"linked_nomination\",\"nomination_transaction_id\":\"")
                        + linked_tx_id + "\"}",
                    true,
                    260),
                "provider parallel stun: linked ice_nomination should apply");
    const IceCandidatePair linked_pair = peer.selected_ice_pair();
    TEST_ASSERT(linked_pair.nomination_transaction_id == linked_tx_id,
                "provider parallel stun: selected pair should preserve linked nomination transaction id");
    return true;
}

bool test_webrtc_peer_session_nomination_signal_polling_and_duplicate_suppression()
{
    WebrtcPeerSession peer(0xCCCCCC04, 90000);
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 50;
    cfg.stun_response_delay_ms = 20;
    cfg.stun_timeout_ms = 80;
    cfg.stun_retry_interval_ms = 5;
    cfg.stun_max_retransmits = 2;
    cfg.force_stun_timeout = false;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    WebrtcSignalingBridge::SignalingMessage signal;
    TEST_ASSERT(!peer.poll_ice_nomination_signal(signal),
                "nomination signal polling: no signal should be pending initially");

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:41 1 UDP 2122260223 10.0.0.41 5041 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    120),
                "nomination signal polling: candidate should apply");

    peer.advance_transport(180);
    std::vector<WebrtcSignalingBridge::SignalingMessage> first_batch;
    while (peer.poll_ice_nomination_signal(signal)) {
        first_batch.push_back(signal);
    }
    TEST_ASSERT(!first_batch.empty(),
                "nomination signal polling: at least one nomination signal should be produced after state change");
    bool saw_nominated = false;
    for (const auto &msg : first_batch) {
        TEST_ASSERT(msg.kind == WebrtcSignalingBridge::SignalingMessage::Kind::ice_nomination,
                    "nomination signal polling: all queued messages should be ice_nomination");
        if (msg.ice_nomination_state == IceNominationState::nominated) {
            saw_nominated = true;
            TEST_ASSERT(!msg.nomination_transaction_id.empty(),
                        "nomination signal polling: nominated signal should carry nomination transaction id");
        }
    }
    TEST_ASSERT(saw_nominated,
                "nomination signal polling: should include nominated state signal");

    TEST_ASSERT(!peer.poll_ice_nomination_signal(signal),
                "nomination signal polling: duplicate state should not re-emit signal");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"forced_failed\",\"nomination_transaction_id\":\"unknown-tx\"}",
                    true,
                    200),
                "nomination signal polling: forced failed nomination should apply");

    std::vector<WebrtcSignalingBridge::SignalingMessage> second_batch;
    while (peer.poll_ice_nomination_signal(signal)) {
        second_batch.push_back(signal);
    }
    TEST_ASSERT(!second_batch.empty(),
                "nomination signal polling: state transition to failed should emit signal");
    bool saw_failed = false;
    for (const auto &msg : second_batch) {
        if (msg.ice_nomination_state == IceNominationState::failed) {
            saw_failed = true;
        }
    }
    TEST_ASSERT(saw_failed,
                "nomination signal polling: emitted batch should include failed state");

    TEST_ASSERT(!peer.poll_ice_nomination_signal(signal),
                "nomination signal polling: consumed signal should not be returned again");
    return true;
}

bool test_webrtc_peer_session_nomination_signal_polling_queue_order()
{
    WebrtcPeerSession peer(0xCCCCCC05, 90000);
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 40;
    cfg.stun_response_delay_ms = 10;
    cfg.stun_timeout_ms = 80;
    cfg.stun_retry_interval_ms = 5;
    cfg.stun_max_retransmits = 2;
    cfg.force_stun_timeout = false;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:51 1 UDP 2122260223 10.0.0.51 5051 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    110),
                "nomination queue: candidate should apply");
    peer.advance_transport(180);

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"queue_failed\",\"nomination_transaction_id\":\"queue-id-1\"}",
                    true,
                    200),
                "nomination queue: failed nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"queue_nominated\",\"nomination_transaction_id\":\"queue-id-2\"}",
                    true,
                    220),
                "nomination queue: nominated nomination should apply");

    std::vector<WebrtcSignalingBridge::SignalingMessage> drained;
    WebrtcSignalingBridge::SignalingMessage msg;
    while (peer.poll_ice_nomination_signal(msg)) {
        drained.push_back(msg);
    }
    TEST_ASSERT(drained.size() >= 3,
                "nomination queue: should contain multiple queued nomination transitions");

    int failed_index = -1;
    int nominated_index = -1;
    for (std::size_t i = 0; i < drained.size(); ++i) {
        if (drained[i].ice_nomination_state == IceNominationState::failed
            && failed_index < 0) {
            failed_index = static_cast<int>(i);
        }
        if (drained[i].ice_nomination_state == IceNominationState::nominated
            && drained[i].nomination_transaction_id == "queue-id-2") {
            nominated_index = static_cast<int>(i);
        }
    }
    TEST_ASSERT(failed_index >= 0,
                "nomination queue: should include forced failed transition");
    TEST_ASSERT(nominated_index >= 0,
                "nomination queue: should include forced nominated transition with queue-id-2");
    TEST_ASSERT(failed_index < nominated_index,
                "nomination queue: FIFO should preserve failed then nominated transition order");

    WebrtcSignalingBridge::SignalingMessage none;
    TEST_ASSERT(!peer.poll_ice_nomination_signal(none),
                "nomination queue: queue should be empty after consuming all signals");
    return true;
}

bool test_webrtc_peer_session_nomination_signal_queue_capacity_and_drop_counter()
{
    WebrtcPeerSession peer(0xCCCCCC06, 90000);
    NominationSignalQueueConfig qcfg;
    qcfg.max_pending_signals = 8;
    peer.set_nomination_signal_queue_config(qcfg);
    TEST_ASSERT(peer.nomination_signal_queue_config().max_pending_signals == 8,
                "nomination capacity: queue config should be applied");
    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 30;
    cfg.stun_response_delay_ms = 8;
    cfg.stun_timeout_ms = 50;
    cfg.stun_retry_interval_ms = 4;
    cfg.stun_max_retransmits = 2;
    cfg.force_stun_timeout = false;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:61 1 UDP 2122260223 10.0.0.61 5061 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    110),
                "nomination capacity: baseline candidate should apply");
    peer.advance_transport(150);

    for (int i = 0; i < 25; ++i) {
        const bool failed_phase = (i % 2) == 0;
        const std::string state = failed_phase ? "failed" : "nominated";
        const std::string reason_text = failed_phase ? "capacity_failed" : "capacity_nominated";
        const std::string tx_id = std::string("cap-id-") + std::to_string(i);
        const std::string json = std::string("{\"type\":\"ice_nomination\",\"state\":\"")
                                 + state
                                 + "\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\""
                                 + reason_text
                                 + "\",\"nomination_transaction_id\":\""
                                 + tx_id
                                 + "\"}";
        TEST_ASSERT(peer.apply_signaling_json(json, true, static_cast<uint64_t>(200 + i * 5)),
                    "nomination capacity: forced nomination transition should apply");
    }

    const auto snap = peer.snapshot();
    TEST_ASSERT(snap.pending_ice_nomination_signal_count <= 8,
                "nomination capacity: pending queue count should respect queue capacity");
    TEST_ASSERT(snap.peak_pending_ice_nomination_signal_count <= 8,
                "nomination capacity: peak queue count should respect queue capacity");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_count > 0,
                "nomination capacity: drop counter should increase when queue overflows");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_overflow_count > 0,
                "nomination capacity: overflow drop counter should increase when queue overflows");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_trim_count == 0,
                "nomination capacity: trim drop counter should remain zero without queue reconfiguration trim");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_count
                    == snap.dropped_ice_nomination_signal_overflow_count
                           + snap.dropped_ice_nomination_signal_trim_count,
                "nomination capacity: total drop counter should equal overflow+trim counters");

    const std::string snap_json = peer.snapshot_json();
    WebrtcPeerSessionSnapshot parsed;
    TEST_ASSERT(peer.parse_snapshot_json(snap_json, parsed),
                "nomination capacity: snapshot JSON should parse");
    TEST_ASSERT(parsed.pending_ice_nomination_signal_count == snap.pending_ice_nomination_signal_count,
                "nomination capacity: parsed pending queue count should match");
    TEST_ASSERT(parsed.peak_pending_ice_nomination_signal_count == snap.peak_pending_ice_nomination_signal_count,
                "nomination capacity: parsed peak queue count should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_count == snap.dropped_ice_nomination_signal_count,
                "nomination capacity: parsed drop counter should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_overflow_count == snap.dropped_ice_nomination_signal_overflow_count,
                "nomination capacity: parsed overflow drop counter should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_trim_count == snap.dropped_ice_nomination_signal_trim_count,
                "nomination capacity: parsed trim drop counter should match");

    std::size_t drained_count = 0;
    WebrtcSignalingBridge::SignalingMessage msg;
    while (peer.poll_ice_nomination_signal(msg)) {
        ++drained_count;
    }
    TEST_ASSERT(drained_count == snap.pending_ice_nomination_signal_count,
                "nomination capacity: drained count should match pending queue count in snapshot");
    return true;
}

bool test_webrtc_peer_session_nomination_signal_queue_config_clamps_and_trims()
{
    WebrtcPeerSession peer(0xCCCCCC07, 90000);
    NominationSignalQueueConfig cfg;
    cfg.max_pending_signals = 0;
    peer.set_nomination_signal_queue_config(cfg);
    TEST_ASSERT(peer.nomination_signal_queue_config().max_pending_signals == 1,
                "nomination queue config: max_pending_signals should clamp to 1");

    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig pcfg;
    pcfg.emit_srflx_candidate = true;
    pcfg.gather_delay_ms = 10;
    pcfg.checklist_delay_ms = 30;
    pcfg.stun_response_delay_ms = 8;
    pcfg.stun_timeout_ms = 50;
    pcfg.stun_retry_interval_ms = 4;
    pcfg.stun_max_retransmits = 2;
    pcfg.force_stun_timeout = false;
    provider->set_config(pcfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:71 1 UDP 2122260223 10.0.0.71 5071 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    110),
                "nomination queue config: candidate should apply");
    peer.advance_transport(150);

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"trim_failed\",\"nomination_transaction_id\":\"trim-id-1\"}",
                    true,
                    200),
                "nomination queue config: failed nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"trim_nominated\",\"nomination_transaction_id\":\"trim-id-2\"}",
                    true,
                    220),
                "nomination queue config: nominated nomination should apply");

    NominationSignalQueueConfig expand_cfg;
    expand_cfg.max_pending_signals = 4;
    peer.set_nomination_signal_queue_config(expand_cfg);

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"trim_failed_2\",\"nomination_transaction_id\":\"trim-id-3\"}",
                    true,
                    240),
                "nomination queue config: second failed nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"trim_nominated_2\",\"nomination_transaction_id\":\"trim-id-4\"}",
                    true,
                    260),
                "nomination queue config: second nominated nomination should apply");

    NominationSignalQueueConfig shrink_cfg;
    shrink_cfg.max_pending_signals = 1;
    peer.set_nomination_signal_queue_config(shrink_cfg);

    auto snap = peer.snapshot();
    TEST_ASSERT(snap.pending_ice_nomination_signal_count == 1,
                "nomination queue config: pending queue should be trimmed to one entry");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_count > 0,
                "nomination queue config: drop counter should increase after trimming/overflow");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_trim_count > 0,
                "nomination queue config: trim drop counter should increase after config trimming");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_count
                    == snap.dropped_ice_nomination_signal_overflow_count
                           + snap.dropped_ice_nomination_signal_trim_count,
                "nomination queue config: total drop counter should equal overflow+trim counters");

    WebrtcSignalingBridge::SignalingMessage only;
    TEST_ASSERT(peer.poll_ice_nomination_signal(only),
                "nomination queue config: one signal should be available");
    TEST_ASSERT(only.ice_nomination_state == IceNominationState::nominated,
                "nomination queue config: newest signal should be retained after trimming");
    TEST_ASSERT(!peer.poll_ice_nomination_signal(only),
                "nomination queue config: queue should be empty after single poll");
    return true;
}

bool test_webrtc_peer_session_signal_queue_runtime_config_unified_surface()
{
    WebrtcPeerSession peer(0xCCCCCC10, 90000);

    SignalQueueRuntimeConfig cfg;
    cfg.nomination_max_pending_signals = 0;
    cfg.diagnostics_keep_latest_only = false;
    cfg.diagnostics_max_pending_signals = 0;
    peer.set_signal_queue_runtime_config(cfg);

    SignalQueueRuntimeConfig applied = peer.signal_queue_runtime_config();
    TEST_ASSERT(applied.nomination_max_pending_signals == 1,
                "queue runtime config: nomination max should clamp to 1");
    TEST_ASSERT(!applied.diagnostics_keep_latest_only,
                "queue runtime config: diagnostics keep_latest_only should match config");
    TEST_ASSERT(applied.diagnostics_max_pending_signals == 1,
                "queue runtime config: diagnostics max should clamp to 1");

    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig pcfg;
    pcfg.emit_srflx_candidate = true;
    pcfg.gather_delay_ms = 10;
    pcfg.checklist_delay_ms = 30;
    pcfg.stun_response_delay_ms = 8;
    pcfg.stun_timeout_ms = 50;
    pcfg.stun_retry_interval_ms = 4;
    pcfg.stun_max_retransmits = 2;
    pcfg.force_stun_timeout = false;
    provider->set_config(pcfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:91 1 UDP 2122260223 10.0.0.91 5091 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    110),
                "queue runtime config: candidate should apply");
    peer.advance_transport(150);

    for (int i = 0; i < 6; ++i) {
        const bool failed_phase = (i % 2) == 0;
        const std::string state = failed_phase ? "failed" : "nominated";
        const std::string reason_text = failed_phase ? "runtime_cfg_failed" : "runtime_cfg_nominated";
        const std::string tx_id = std::string("runtime-cfg-") + std::to_string(i);
        const std::string json = std::string("{\"type\":\"ice_nomination\",\"state\":\"")
                                 + state
                                 + "\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\""
                                 + reason_text
                                 + "\",\"nomination_transaction_id\":\""
                                 + tx_id
                                 + "\"}";
        TEST_ASSERT(peer.apply_signaling_json(json, true, static_cast<uint64_t>(200 + i * 10)),
                    "queue runtime config: forced nomination should apply in fifo mode");
    }

    auto snap = peer.snapshot();
    TEST_ASSERT(snap.pending_ice_nomination_signal_count <= 1,
                "queue runtime config: nomination queue should respect runtime max=1");
    TEST_ASSERT(snap.pending_diagnostics_signal_count <= 1,
                "queue runtime config: diagnostics queue should respect runtime max=1 fifo");
    TEST_ASSERT(snap.dropped_ice_nomination_signal_overflow_count > 0,
                "queue runtime config: nomination overflow counter should increase");
    TEST_ASSERT(snap.dropped_diagnostics_signal_overflow_count > 0,
                "queue runtime config: diagnostics overflow counter should increase in fifo mode");

    SignalQueueRuntimeConfig cfg2;
    cfg2.nomination_max_pending_signals = 3;
    cfg2.diagnostics_keep_latest_only = true;
    cfg2.diagnostics_max_pending_signals = 5;
    peer.set_signal_queue_runtime_config(cfg2);

    applied = peer.signal_queue_runtime_config();
    TEST_ASSERT(applied.nomination_max_pending_signals == 3,
                "queue runtime config: nomination max should update via unified config");
    TEST_ASSERT(applied.diagnostics_keep_latest_only,
                "queue runtime config: diagnostics keep_latest_only should update via unified config");
    TEST_ASSERT(applied.diagnostics_max_pending_signals == 5,
                "queue runtime config: diagnostics max should update via unified config");
    TEST_ASSERT(!applied.diagnostics_emit_flat_compat_fields,
                "queue runtime config: diagnostics emit_flat_compat_fields should default false");
    TEST_ASSERT(!applied.diagnostics_release_mode_strict_v2,
                "queue runtime config: diagnostics release_mode_strict_v2 should default false");

    for (int i = 6; i < 10; ++i) {
        const bool failed_phase = (i % 2) == 0;
        const std::string state = failed_phase ? "failed" : "nominated";
        const std::string reason_text = failed_phase ? "runtime_cfg_latest_failed" : "runtime_cfg_latest_nominated";
        const std::string tx_id = std::string("runtime-cfg-") + std::to_string(i);
        const std::string json = std::string("{\"type\":\"ice_nomination\",\"state\":\"")
                                 + state
                                 + "\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\""
                                 + reason_text
                                 + "\",\"nomination_transaction_id\":\""
                                 + tx_id
                                 + "\"}";
        TEST_ASSERT(peer.apply_signaling_json(json, true, static_cast<uint64_t>(300 + i * 10)),
                    "queue runtime config: forced nomination should apply in latest-only mode");
    }

    WebrtcSignalingBridge::SignalingMessage diag;
    TEST_ASSERT(peer.poll_diagnostics_signal(diag),
                "queue runtime config: diagnostics message should be available after latest-only updates");
    TEST_ASSERT(diag.diagnostics_policy_keep_latest_only,
                "queue runtime config: diagnostics policy should expose keep_latest_only=true");
    TEST_ASSERT(diag.diagnostics_policy_max_pending_signals == 5,
                "queue runtime config: diagnostics policy should expose diagnostics max=5");
    TEST_ASSERT(diag.diagnostics_policy_nomination_max_pending_signals == 3,
                "queue runtime config: diagnostics policy should expose nomination max=3");
    const std::string diag_json = peer.signaling_bridge().to_signaling_json(diag);
    TEST_ASSERT(diag_json.find("\"policy\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested policy object");
    TEST_ASSERT(diag_json.find("\"schema_version\":\"v2\"") != std::string::npos,
                "queue runtime config: diagnostics JSON should include schema_version v2");
    TEST_ASSERT(diag_json.find("\"emit_flat_compat_fields\":false") != std::string::npos,
                "queue runtime config: diagnostics JSON should include emit_flat_compat_fields=false by default");
    TEST_ASSERT(diag_json.find("\"queues\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested queues object");
    TEST_ASSERT(diag_json.find("\"nomination\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested nomination object");
    TEST_ASSERT(diag_json.find("\"stun\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested stun object");
    TEST_ASSERT(diag_json.find("\"migration\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested migration object");
    TEST_ASSERT(diag_json.find("\"rollout_policy\":{") != std::string::npos,
                "queue runtime config: diagnostics JSON should include nested rollout policy object");
    TEST_ASSERT(!peer.poll_diagnostics_signal(diag),
                "queue runtime config: latest-only mode should keep one diagnostics message");

    SignalQueueRuntimeConfig cfg3 = applied;
    cfg3.diagnostics_emit_flat_compat_fields = false;
    cfg3.diagnostics_release_mode_strict_v2 = true;
    cfg3.diagnostics_rollout_alert_window_seconds = 7200;
    cfg3.diagnostics_rollout_mismatch_count_alert_threshold = 2;
    cfg3.diagnostics_rollout_mismatch_ratio_threshold_ppm = 500;
    peer.set_signal_queue_runtime_config(cfg3);
    applied = peer.signal_queue_runtime_config();
    TEST_ASSERT(!applied.diagnostics_emit_flat_compat_fields,
                "queue runtime config: unified runtime config should disable flat compat emission");
    TEST_ASSERT(applied.diagnostics_release_mode_strict_v2,
                "queue runtime config: unified runtime config should enable release_mode_strict_v2");
    TEST_ASSERT(applied.diagnostics_rollout_alert_window_seconds == 7200,
                "queue runtime config: unified runtime config should update rollout alert window");
    TEST_ASSERT(applied.diagnostics_rollout_mismatch_count_alert_threshold == 2,
                "queue runtime config: unified runtime config should update rollout mismatch count threshold");
    TEST_ASSERT(applied.diagnostics_rollout_mismatch_ratio_threshold_ppm == 500,
                "queue runtime config: unified runtime config should update rollout mismatch ratio threshold");

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_nested_only\",\"nomination_transaction_id\":\"diag-nested-only\"}",
                    true,
                    520),
                "queue runtime config: additional nomination should apply for nested-only emission check");
    TEST_ASSERT(peer.poll_diagnostics_signal(diag),
                "queue runtime config: diagnostics message should be available in nested-only emission mode");
    const std::string diag_json_nested_only = peer.signaling_bridge().to_signaling_json(diag);
    TEST_ASSERT(diag_json_nested_only.find("\"nomination\":{") != std::string::npos,
                "queue runtime config: nested-only emission should still include nomination object");
    TEST_ASSERT(diag_json_nested_only.find("\"ice_nomination_state\":") == std::string::npos,
                "queue runtime config: nested-only emission should omit flat nomination fields");
    TEST_ASSERT(diag_json_nested_only.find("\"queues\":{") != std::string::npos,
                "queue runtime config: nested-only emission should still include queues object");
    TEST_ASSERT(diag_json_nested_only.find("\"pending_nomination_signal_count\":") == std::string::npos,
                "queue runtime config: nested-only emission should omit flat queues fields");
    TEST_ASSERT(diag_json_nested_only.find("\"policy\":{") != std::string::npos,
                "queue runtime config: nested-only emission should still include policy object");
    TEST_ASSERT(diag_json_nested_only.find("\"policy_keep_latest_only\":") == std::string::npos,
                "queue runtime config: nested-only emission should omit flat policy fields");
    TEST_ASSERT(diag_json_nested_only.find("\"migration\":{") != std::string::npos,
                "queue runtime config: nested-only emission should still include migration object");
    TEST_ASSERT(diag_json_nested_only.find("\"v2_flat_duplicate_seen_count\":") != std::string::npos,
                "queue runtime config: nested-only emission should include migration counters");
    TEST_ASSERT(diag_json_nested_only.find("\"schema_version\":\"v2\"") != std::string::npos,
                "queue runtime config: nested-only emission should force schema_version v2");
    TEST_ASSERT(diag_json_nested_only.find("\"emit_flat_compat_fields\":false") != std::string::npos,
                "queue runtime config: nested-only emission should include emit_flat_compat_fields=false");
    TEST_ASSERT(diag_json_nested_only.find("\"release_mode_strict_v2\":true") != std::string::npos,
                "queue runtime config: release mode emission should include release_mode_strict_v2=true");
    TEST_ASSERT(diag_json_nested_only.find("\"rollout_policy\":{") != std::string::npos,
                "queue runtime config: release mode emission should include nested rollout policy object");
    TEST_ASSERT(diag_json_nested_only.find("\"alert_window_seconds\":7200") != std::string::npos,
                "queue runtime config: release mode emission should include rollout alert window");
    TEST_ASSERT(diag_json_nested_only.find("\"mismatch_count_alert_threshold\":2") != std::string::npos,
                "queue runtime config: release mode emission should include rollout mismatch count threshold");
    TEST_ASSERT(diag_json_nested_only.find("\"mismatch_ratio_threshold_ppm\":500") != std::string::npos,
                "queue runtime config: release mode emission should include rollout mismatch ratio threshold");
    TEST_ASSERT(diag_json_nested_only.find("\"current_mismatch_ratio_ppm\":0") != std::string::npos,
                "queue runtime config: release mode emission should include current mismatch ratio signal");
    TEST_ASSERT(diag_json_nested_only.find("\"alert_active\":false") != std::string::npos,
                "queue runtime config: release mode emission should include alert_active=false initially");
    TEST_ASSERT(diag_json_nested_only.find("\"ready_for_progress\":true") != std::string::npos,
                "queue runtime config: release mode emission should include ready_for_progress=true initially");

    snap = peer.snapshot();
    TEST_ASSERT(snap.diagnostics_policy_keep_latest_only,
                "queue runtime config: snapshot should expose keep_latest_only=true");
    TEST_ASSERT(snap.diagnostics_policy_max_pending_signals == 5,
                "queue runtime config: snapshot should expose diagnostics max=5");
    TEST_ASSERT(snap.diagnostics_policy_nomination_max_pending_signals == 3,
                "queue runtime config: snapshot should expose nomination max=3");
    TEST_ASSERT(!snap.diagnostics_emit_flat_compat_fields,
                "queue runtime config: snapshot should expose diagnostics emit_flat_compat_fields=false");
    TEST_ASSERT(snap.diagnostics_release_mode_strict_v2,
                "queue runtime config: snapshot should expose diagnostics release_mode_strict_v2=true");
    TEST_ASSERT(snap.diagnostics_rollout_alert_window_seconds == 7200,
                "queue runtime config: snapshot should expose rollout alert window");
    TEST_ASSERT(snap.diagnostics_rollout_mismatch_count_alert_threshold == 2,
                "queue runtime config: snapshot should expose rollout mismatch count threshold");
    TEST_ASSERT(snap.diagnostics_rollout_mismatch_ratio_threshold_ppm == 500,
                "queue runtime config: snapshot should expose rollout mismatch ratio threshold");
    TEST_ASSERT(snap.diagnostics_rollout_current_mismatch_ratio_ppm == 0,
                "queue runtime config: snapshot should expose current mismatch ratio signal");
    TEST_ASSERT(!snap.diagnostics_rollout_alert_active,
                "queue runtime config: snapshot should expose rollout alert inactive by default");
    TEST_ASSERT(!snap.diagnostics_rollout_progress_blocked,
                "queue runtime config: snapshot should expose rollout progress not blocked by default");
    TEST_ASSERT(snap.diagnostics_rollout_ready_for_progress,
                "queue runtime config: snapshot should expose rollout ready_for_progress by default");
    TEST_ASSERT(snap.dropped_diagnostics_signal_count
                    == snap.dropped_diagnostics_signal_overflow_count
                           + snap.dropped_diagnostics_signal_trim_count,
                "queue runtime config: diagnostics drop total should equal overflow+trim");

    const std::string runtime_cfg_json = peer.signal_queue_runtime_config_json();
    TEST_ASSERT(runtime_cfg_json.find("\"diagnostics_rollout_alert_window_seconds\":7200") != std::string::npos,
                "queue runtime config: runtime config JSON should include rollout alert window");
    TEST_ASSERT(runtime_cfg_json.find("\"diagnostics_rollout_mismatch_count_alert_threshold\":2") != std::string::npos,
                "queue runtime config: runtime config JSON should include rollout mismatch count threshold");
    TEST_ASSERT(runtime_cfg_json.find("\"diagnostics_rollout_mismatch_ratio_threshold_ppm\":500") != std::string::npos,
                "queue runtime config: runtime config JSON should include rollout mismatch ratio threshold");

    SignalQueueRuntimeConfig parsed_runtime_cfg;
    TEST_ASSERT(peer.parse_signal_queue_runtime_config_json(
                    "{\"nomination_max_pending_signals\":0,\"diagnostics_keep_latest_only\":false,"
                    "\"diagnostics_max_pending_signals\":0,\"diagnostics_emit_flat_compat_fields\":true,"
                    "\"diagnostics_release_mode_strict_v2\":false,\"diagnostics_rollout_alert_window_seconds\":0,"
                    "\"diagnostics_rollout_mismatch_count_alert_threshold\":3,"
                    "\"diagnostics_rollout_mismatch_ratio_threshold_ppm\":250}",
                    parsed_runtime_cfg),
                "queue runtime config: parse runtime config JSON should pass");
    TEST_ASSERT(parsed_runtime_cfg.nomination_max_pending_signals == 1,
                "queue runtime config: parsed runtime config should clamp nomination max to >=1");
    TEST_ASSERT(parsed_runtime_cfg.diagnostics_max_pending_signals == 1,
                "queue runtime config: parsed runtime config should clamp diagnostics max to >=1");
    TEST_ASSERT(parsed_runtime_cfg.diagnostics_rollout_alert_window_seconds == 1,
                "queue runtime config: parsed runtime config should clamp rollout alert window to >=1");
    TEST_ASSERT(parsed_runtime_cfg.diagnostics_rollout_mismatch_count_alert_threshold == 3,
                "queue runtime config: parsed runtime config should keep mismatch count threshold");
    TEST_ASSERT(parsed_runtime_cfg.diagnostics_rollout_mismatch_ratio_threshold_ppm == 250,
                "queue runtime config: parsed runtime config should keep mismatch ratio threshold");

    TEST_ASSERT(!peer.parse_signal_queue_runtime_config_json(
                    "{\"diagnostics_rollout_alert_window_seconds\":\"bad\"}",
                    parsed_runtime_cfg),
                "queue runtime config: parse runtime config JSON should reject invalid field types");

    peer.set_signal_queue_runtime_config(parsed_runtime_cfg);
    applied = peer.signal_queue_runtime_config();
    TEST_ASSERT(applied.nomination_max_pending_signals == 1,
                "queue runtime config: parsed config should apply nomination max");
    TEST_ASSERT(applied.diagnostics_max_pending_signals == 1,
                "queue runtime config: parsed config should apply diagnostics max");
    TEST_ASSERT(applied.diagnostics_rollout_alert_window_seconds == 1,
                "queue runtime config: parsed config should apply rollout alert window");
    TEST_ASSERT(applied.diagnostics_rollout_mismatch_count_alert_threshold == 3,
                "queue runtime config: parsed config should apply rollout mismatch count threshold");
    TEST_ASSERT(applied.diagnostics_rollout_mismatch_ratio_threshold_ppm == 250,
                "queue runtime config: parsed config should apply rollout mismatch ratio threshold");

    const std::string rollout_health_ready_json = peer.rollout_health_json();
    TEST_ASSERT(rollout_health_ready_json.find("\"rollout_ready_for_progress\":true") != std::string::npos,
                "queue runtime config: rollout health should expose ready_for_progress=true initially");

    TEST_ASSERT(!peer.apply_signaling_json(
                    "{\"type\":\"diagnostics\",\"scope\":\"peer_session\",\"schema_version\":\"v2\","
                    "\"nomination\":{\"state\":\"nominated\",\"has_selected_pair\":true,"
                    "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_mismatch\"},"
                    "\"stun\":{\"last_ice_error\":\"\",\"transaction_count\":1,\"has_last_transaction\":true,"
                    "\"last_transaction_id\":\"tx-m\",\"last_transaction_state\":\"response_received\"},"
                    "\"queues\":{\"nomination\":{\"pending_signal_count\":1,\"dropped_signal_count\":0,"
                    "\"dropped_signal_overflow_count\":0,\"dropped_signal_trim_count\":0},"
                    "\"diagnostics\":{\"pending_signal_count\":1,\"dropped_signal_count\":0,"
                    "\"dropped_signal_overflow_count\":0,\"dropped_signal_trim_count\":0}},"
                    "\"policy\":{\"keep_latest_only\":true,\"max_pending_signals\":1,"
                    "\"nomination_max_pending_signals\":1},"
                    "\"ice_nomination_state\":\"failed\",\"has_selected_pair\":true,"
                    "\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_mismatch\"}",
                    true,
                    900),
                "queue runtime config: mismatched duplicate diagnostics should be rejected and counted");

    const std::string rollout_health_blocked_json = peer.rollout_health_json();
    TEST_ASSERT(rollout_health_blocked_json.find("\"rollout_alert_active\":false") != std::string::npos,
                "queue runtime config: rollout health should keep alert_active=false when count threshold is not crossed");
    TEST_ASSERT(rollout_health_blocked_json.find("\"rollout_progress_blocked\":true") != std::string::npos,
                "queue runtime config: rollout health should expose progress_blocked=true after mismatch");
    TEST_ASSERT(rollout_health_blocked_json.find("\"rollout_ready_for_progress\":false") != std::string::npos,
                "queue runtime config: rollout health should expose ready_for_progress=false after mismatch");
    return true;
}

bool test_webrtc_peer_session_diagnostics_signal_reflects_runtime()
{
    WebrtcPeerSession peer(0xCCCCCC08, 90000);
    NominationSignalQueueConfig qcfg;
    qcfg.max_pending_signals = 3;
    peer.set_nomination_signal_queue_config(qcfg);

    auto provider = std::make_shared<MockIceProvider>();
    MockIceProviderConfig cfg;
    cfg.emit_srflx_candidate = true;
    cfg.gather_delay_ms = 10;
    cfg.checklist_delay_ms = 30;
    cfg.stun_response_delay_ms = 8;
    cfg.stun_timeout_ms = 50;
    cfg.stun_retry_interval_ms = 4;
    cfg.stun_max_retransmits = 2;
    cfg.force_stun_timeout = false;
    provider->set_config(cfg);
    peer.set_ice_provider(provider);

    peer.start_ice_gathering(100);
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"candidate\",\"candidate\":\"candidate:81 1 UDP 2122260223 10.0.0.81 5081 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                    true,
                    110),
                "diagnostics signal: candidate should apply");
    peer.advance_transport(150);

    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_failed\",\"nomination_transaction_id\":\"diag-id-1\"}",
                    true,
                    200),
                "diagnostics signal: failed nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_nominated\",\"nomination_transaction_id\":\"diag-id-2\"}",
                    true,
                    220),
                "diagnostics signal: nominated nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"failed\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_failed_2\",\"nomination_transaction_id\":\"diag-id-3\"}",
                    true,
                    240),
                "diagnostics signal: second failed nomination should apply");
    TEST_ASSERT(peer.apply_signaling_json(
                    "{\"type\":\"ice_nomination\",\"state\":\"nominated\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\"diag_nominated_2\",\"nomination_transaction_id\":\"diag-id-4\"}",
                    true,
                    260),
                "diagnostics signal: second nominated nomination should apply");

    WebrtcSignalingBridge::SignalingMessage diagnostics;
    TEST_ASSERT(peer.poll_diagnostics_signal(diagnostics),
                "diagnostics signal: diagnostics message should be available");
    TEST_ASSERT(diagnostics.kind == WebrtcSignalingBridge::SignalingMessage::Kind::diagnostics,
                "diagnostics signal: kind should be diagnostics");
    TEST_ASSERT(diagnostics.diagnostics_scope == "peer_session",
                "diagnostics signal: scope should be peer_session");
    TEST_ASSERT(diagnostics.diagnostics_dropped_nomination_signal_count > 0,
                "diagnostics signal: dropped nomination counter should be populated");
    TEST_ASSERT(diagnostics.diagnostics_dropped_nomination_signal_count
                    == diagnostics.diagnostics_dropped_nomination_signal_overflow_count
                           + diagnostics.diagnostics_dropped_nomination_signal_trim_count,
                "diagnostics signal: dropped total should equal overflow+trim");
    TEST_ASSERT(diagnostics.diagnostics_pending_nomination_signal_count <= 3,
                "diagnostics signal: pending nomination count should respect configured queue bound");
    TEST_ASSERT(diagnostics.diagnostics_dropped_diagnostics_signal_count
                    == diagnostics.diagnostics_dropped_diagnostics_signal_overflow_count
                           + diagnostics.diagnostics_dropped_diagnostics_signal_trim_count,
                "diagnostics signal: diagnostics drop total should equal overflow+trim");
    TEST_ASSERT(diagnostics.diagnostics_policy_keep_latest_only,
                "diagnostics signal: default diagnostics queue policy should be keep_latest_only");
    TEST_ASSERT(diagnostics.diagnostics_policy_max_pending_signals == 8,
                "diagnostics signal: default diagnostics queue max should be 8");
    TEST_ASSERT(diagnostics.diagnostics_policy_nomination_max_pending_signals == 3,
                "diagnostics signal: nomination queue max should reflect runtime config");

    TEST_ASSERT(!peer.poll_diagnostics_signal(diagnostics),
                "diagnostics signal: only latest diagnostics message should be kept");

    DiagnosticsSignalQueueConfig dcfg;
    dcfg.keep_latest_only = false;
    dcfg.max_pending_signals = 3;
    peer.set_diagnostics_signal_queue_config(dcfg);
    TEST_ASSERT(!peer.diagnostics_signal_queue_config().keep_latest_only,
                "diagnostics signal: queue config should disable latest-only mode");

    for (int i = 0; i < 6; ++i) {
        const bool failed_phase = (i % 2) == 0;
        const std::string state = failed_phase ? "failed" : "nominated";
        const std::string reason_text = failed_phase ? "diag_fifo_failed" : "diag_fifo_nominated";
        const std::string tx_id = std::string("diag-fifo-") + std::to_string(i);
        const std::string json = std::string("{\"type\":\"ice_nomination\",\"state\":\"")
                                 + state
                                 + "\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\""
                                 + reason_text
                                 + "\",\"nomination_transaction_id\":\""
                                 + tx_id
                                 + "\"}";
        TEST_ASSERT(peer.apply_signaling_json(json, true, static_cast<uint64_t>(300 + i * 10)),
                    "diagnostics signal: forced nomination should apply in fifo mode");
    }

    std::vector<WebrtcSignalingBridge::SignalingMessage> diag_msgs;
    while (peer.poll_diagnostics_signal(diagnostics)) {
        diag_msgs.push_back(diagnostics);
    }
    TEST_ASSERT(diag_msgs.size() <= 3,
                "diagnostics signal: diagnostics FIFO queue should respect configured max size");
    TEST_ASSERT(!diag_msgs.empty(),
                "diagnostics signal: diagnostics FIFO queue should emit messages");
    TEST_ASSERT(!diag_msgs.back().diagnostics_policy_keep_latest_only,
                "diagnostics signal: FIFO diagnostics message should expose keep_latest_only=false");
    TEST_ASSERT(diag_msgs.back().diagnostics_policy_max_pending_signals == 3,
                "diagnostics signal: FIFO diagnostics message should expose configured diagnostics max");
    TEST_ASSERT(diag_msgs.back().diagnostics_policy_nomination_max_pending_signals == 3,
                "diagnostics signal: FIFO diagnostics message should expose nomination queue max");

    const auto snap = peer.snapshot();
    TEST_ASSERT(snap.pending_diagnostics_signal_count == 0,
                "diagnostics signal: pending diagnostics count should be zero after draining queue");
    TEST_ASSERT(snap.peak_pending_diagnostics_signal_count <= 3,
                "diagnostics signal: diagnostics peak should respect configured max size");
    TEST_ASSERT(snap.dropped_diagnostics_signal_count > 0,
                "diagnostics signal: diagnostics drop counter should increase in fifo overflow");
    TEST_ASSERT(snap.dropped_diagnostics_signal_count
                    == snap.dropped_diagnostics_signal_overflow_count
                           + snap.dropped_diagnostics_signal_trim_count,
                "diagnostics signal: diagnostics drop total should equal overflow+trim");
    return true;
}

bool test_webrtc_peer_session_diagnostics_queue_mode_transition_matrix()
{
    WebrtcPeerSession peer(0xCCCCCC09, 90000);

    DiagnosticsSignalQueueConfig dcfg;
    dcfg.keep_latest_only = false;
    dcfg.max_pending_signals = 4;
    peer.set_diagnostics_signal_queue_config(dcfg);

    auto emit_nomination = [&](int index) {
        const bool failed_phase = (index % 2) == 0;
        const std::string state = failed_phase ? "failed" : "nominated";
        const std::string reason_text = failed_phase ? "diag_matrix_failed" : "diag_matrix_nominated";
        const std::string tx_id = std::string("diag-matrix-") + std::to_string(index);
        const std::string json = std::string("{\"type\":\"ice_nomination\",\"state\":\"")
                                 + state
                                 + "\",\"selected_pair_reason\":\"forced_by_signal\",\"selected_pair_reason_text\":\""
                                 + reason_text
                                 + "\",\"nomination_transaction_id\":\""
                                 + tx_id
                                 + "\"}";
        return peer.apply_signaling_json(json, true, static_cast<uint64_t>(1000 + index * 10));
    };

    for (int i = 0; i < 8; ++i) {
        TEST_ASSERT(emit_nomination(i),
                    "diagnostics matrix: forced nomination should apply in FIFO mode");
    }

    const auto fifo_overflow_snap = peer.snapshot();
    TEST_ASSERT(fifo_overflow_snap.pending_diagnostics_signal_count <= 4,
                "diagnostics matrix: FIFO pending diagnostics should respect max size");
    TEST_ASSERT(fifo_overflow_snap.dropped_diagnostics_signal_overflow_count > 0,
                "diagnostics matrix: FIFO overflow should increase overflow counter");

    dcfg.keep_latest_only = false;
    dcfg.max_pending_signals = 2;
    peer.set_diagnostics_signal_queue_config(dcfg);

    const auto fifo_trim_snap = peer.snapshot();
    TEST_ASSERT(fifo_trim_snap.pending_diagnostics_signal_count <= 2,
                "diagnostics matrix: shrinking FIFO config should trim pending queue");
    TEST_ASSERT(fifo_trim_snap.dropped_diagnostics_signal_trim_count > 0,
                "diagnostics matrix: shrinking FIFO config should increase trim counter");

    dcfg.keep_latest_only = true;
    dcfg.max_pending_signals = 7;
    peer.set_diagnostics_signal_queue_config(dcfg);

    TEST_ASSERT(peer.diagnostics_signal_queue_config().keep_latest_only,
                "diagnostics matrix: keep-latest-only mode should be enabled");
    TEST_ASSERT(peer.diagnostics_signal_queue_config().max_pending_signals == 7,
                "diagnostics matrix: keep-latest-only mode should preserve configured max field");

    for (int i = 8; i < 12; ++i) {
        TEST_ASSERT(emit_nomination(i),
                    "diagnostics matrix: forced nomination should apply in latest-only mode");
    }

    WebrtcSignalingBridge::SignalingMessage last_diag;
    TEST_ASSERT(peer.poll_diagnostics_signal(last_diag),
                "diagnostics matrix: diagnostics message should be available");
    TEST_ASSERT(last_diag.diagnostics_policy_keep_latest_only,
                "diagnostics matrix: diagnostics message should expose keep_latest_only=true");
    TEST_ASSERT(last_diag.diagnostics_policy_max_pending_signals == 7,
                "diagnostics matrix: diagnostics message should expose latest-only configured max");
    TEST_ASSERT(!peer.poll_diagnostics_signal(last_diag),
                "diagnostics matrix: latest-only mode should retain at most one diagnostics message");

    const auto final_snap = peer.snapshot();
    TEST_ASSERT(final_snap.pending_diagnostics_signal_count == 0,
                "diagnostics matrix: diagnostics queue should be empty after drain");
    TEST_ASSERT(final_snap.dropped_diagnostics_signal_overflow_count > 0,
                "diagnostics matrix: overflow counter should remain populated");
    TEST_ASSERT(final_snap.dropped_diagnostics_signal_trim_count > 0,
                "diagnostics matrix: trim counter should remain populated");
    TEST_ASSERT(final_snap.dropped_diagnostics_signal_count
                    == final_snap.dropped_diagnostics_signal_overflow_count
                           + final_snap.dropped_diagnostics_signal_trim_count,
                "diagnostics matrix: final diagnostics drop total should equal overflow+trim");
    return true;
}

bool test_webrtc_transport_bridge_rtp_rtcp_pipeline()
{
    const uint32_t local_ssrc = 0xABCDEF01;
    const uint32_t remote_ssrc = 0x10203040;

    WebrtcTransportBridge bridge(local_ssrc, 90000);

    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 3000, 0), 0), "first RTP packet should be accepted");
    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 3002, 6000), 66), "second RTP packet should be accepted");
    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 3001, 3000), 33), "third RTP packet should be accepted");

    bridge.rtcp_session().on_sender_activity(0x1234567890ABCDEFull, 777, 12, 1200);

    const RtcpPacket rr = bridge.build_receiver_report();
    const RtcpPacket sr = bridge.build_sender_report();

    TEST_ASSERT(rr.kind == RtcpPacket::Kind::receiver_report, "RR kind should match");
    TEST_ASSERT(rr.receiver_report.ssrc == local_ssrc, "RR sender SSRC should match local");
    TEST_ASSERT(rr.receiver_report.report_blocks.size() == 1, "RR should have one block");
    TEST_ASSERT(rr.receiver_report.report_blocks[0].ssrc == remote_ssrc, "RR block SSRC should match remote");

    TEST_ASSERT(sr.kind == RtcpPacket::Kind::sender_report, "SR kind should match");
    TEST_ASSERT(sr.sender_report.ssrc == local_ssrc, "SR sender SSRC should match local");
    TEST_ASSERT(sr.sender_report.packet_count == 12, "SR packet_count should match sender activity");
    TEST_ASSERT(sr.sender_report.octet_count == 1200, "SR octet_count should match sender activity");
    TEST_ASSERT(sr.sender_report.report_blocks.size() == 1, "SR should have one block");
    TEST_ASSERT(sr.sender_report.report_blocks[0].ssrc == remote_ssrc, "SR block SSRC should match remote");
    return true;
}

bool test_webrtc_transport_bridge_rtcp_scheduler_deterministic_tick()
{
    const uint32_t local_ssrc = 0xCAFEBABE;
    const uint32_t remote_ssrc = 0x13572468;

    WebrtcTransportBridge bridge(local_ssrc, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 3;
    config.rr_snapshot_reset_interval_reports = 2;
    bridge.set_rtcp_schedule_config(config);

    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 1000, 0), 0), "scheduler RTP packet 1 should be accepted");
    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 1002, 6000), 66), "scheduler RTP packet 2 should be accepted");
    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 1001, 3000), 33), "scheduler RTP packet 3 should be accepted");

    bridge.on_sender_activity(0x0101010102020202ull, 999, 5, 500);
    bridge.reset_rtcp_schedule(0);

    RtcpPacket pkt;
    TEST_ASSERT(!bridge.poll_scheduled_rtcp(50, pkt), "no packet should be emitted before first due time");

    TEST_ASSERT(bridge.poll_scheduled_rtcp(100, pkt), "packet should be emitted at first due time");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "first scheduled packet should be SR");

    TEST_ASSERT(bridge.poll_scheduled_rtcp(200, pkt), "packet should be emitted at second due time");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "second scheduled packet should be RR");
    TEST_ASSERT(pkt.receiver_report.report_blocks.size() == 1, "scheduled RR should contain one block");

    TEST_ASSERT(bridge.poll_scheduled_rtcp(300, pkt), "packet should be emitted at third due time");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "third scheduled packet should be RR");

    TEST_ASSERT(bridge.poll_scheduled_rtcp(400, pkt), "packet should be emitted at fourth due time");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "fourth scheduled packet should be SR with period 3");

    TEST_ASSERT(!bridge.poll_scheduled_rtcp(450, pkt), "no extra packet between due ticks");
    return true;
}

bool test_webrtc_transport_bridge_schedule_snapshot_reset_effect()
{
    const uint32_t local_ssrc = 0xDEADBEEF;
    const uint32_t remote_ssrc = 0x24681357;

    WebrtcTransportBridge bridge(local_ssrc, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 100;
    config.rr_snapshot_reset_interval_reports = 2;
    bridge.set_rtcp_schedule_config(config);

    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 2000, 0), 0), "round1 packet should be accepted");
    bridge.reset_rtcp_schedule(0);

    RtcpPacket pkt;
    TEST_ASSERT(bridge.poll_scheduled_rtcp(100, pkt), "first RR should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "first packet should be RR");
    TEST_ASSERT(pkt.receiver_report.report_blocks[0].fraction_lost == 0, "first RR interval should be baseline 0");

    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 2002, 6000), 66), "round2 packet should be accepted with gap");
    TEST_ASSERT(bridge.poll_scheduled_rtcp(200, pkt), "second RR should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "second packet should be RR");
    TEST_ASSERT(pkt.receiver_report.report_blocks[0].fraction_lost == 128, "second RR should report interval loss");

    TEST_ASSERT(bridge.on_rtp_packet_received(make_rtp(remote_ssrc, 2003, 9000), 99), "round3 packet should be accepted");
    TEST_ASSERT(bridge.poll_scheduled_rtcp(300, pkt), "third RR should emit after snapshot reset");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "third packet should be RR");
    TEST_ASSERT(pkt.receiver_report.report_blocks[0].fraction_lost == 0, "RR after reset should restart baseline at 0");
    return true;
}

bool test_webrtc_transport_bridge_srtp_hooks_and_failures()
{
    const uint32_t local_ssrc = 0xDEADCAFE;
    const uint32_t remote_ssrc = 0x12344321;

    WebrtcTransportBridge bridge(local_ssrc, 90000);

    MockSrtpContext *raw_ctx = new MockSrtpContext();
    std::shared_ptr<SrtpContext> ctx(raw_ctx);
    bridge.set_srtp_context(ctx);
    TEST_ASSERT(bridge.has_srtp_context(), "SRTP context should be installed");

    RtcPacket rtp = make_rtp(remote_ssrc, 42, 3000);
    TEST_ASSERT(bridge.on_srtp_rtp_packet_received(rtp, 100), "SRTP RTP receive should pass with default mock");
    TEST_ASSERT(raw_ctx->unprotect_rtp_calls() == 1, "unprotect_rtp should be called once");

    RtcpPacket rr = bridge.build_receiver_report();
    TEST_ASSERT(bridge.protect_rtcp_packet(rr), "manual protect_rtcp should pass");
    TEST_ASSERT(raw_ctx->protect_rtcp_calls() == 1, "protect_rtcp should be called once");

    MockSrtpConfig cfg = raw_ctx->config();
    cfg.fail_unprotect_rtp = true;
    raw_ctx->set_config(cfg);
    TEST_ASSERT(!bridge.on_srtp_rtp_packet_received(rtp, 101), "SRTP RTP receive should fail when unprotect fails");

    cfg.fail_unprotect_rtp = false;
    cfg.fail_protect_rtcp = true;
    raw_ctx->set_config(cfg);
    bridge.reset_rtcp_schedule(0);
    bridge.on_sender_activity(0x0102030405060708ull, 123, 1, 100);
    RtcpPacket out;
    TEST_ASSERT(!bridge.poll_scheduled_rtcp(1000, out), "scheduled RTCP should fail when protect_rtcp fails");
    return true;
}

bool test_webrtc_peer_session_scheduler_activation_by_signaling_state()
{
    const uint32_t local_ssrc = 0x41414141;
    const uint32_t remote_ssrc = 0x42424242;

    WebrtcPeerSession peer(local_ssrc, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 1000, 0), 0), "peer RTP should be accepted before signaling ready");

    RtcpPacket pkt;
    TEST_ASSERT(!peer.poll_scheduled_rtcp(100, pkt), "scheduler should stay inactive before stable state");

    WebrtcSignalingBridge::SignalingMessage remote_offer;
    remote_offer.kind = WebrtcSignalingBridge::SignalingMessage::Kind::offer;
    remote_offer.description.type = SdpType::offer;
    remote_offer.description.sdp = valid_offer_sdp();

    WebrtcSignalingBridge::SignalingMessage local_answer;
    local_answer.kind = WebrtcSignalingBridge::SignalingMessage::Kind::answer;
    local_answer.description.type = SdpType::answer;
    local_answer.description.sdp = valid_answer_sdp();

    TEST_ASSERT(peer.apply_signaling_message(remote_offer, true, 100), "remote offer should apply");
    TEST_ASSERT(peer.signaling_state() == SignalingState::have_remote_offer, "state should be have_remote_offer");
    TEST_ASSERT(!peer.poll_scheduled_rtcp(200, pkt), "scheduler should still be inactive before stable");

    TEST_ASSERT(peer.apply_signaling_message(local_answer, false, 200), "local answer should apply");
    TEST_ASSERT(peer.signaling_state() == SignalingState::stable, "state should become stable");
    TEST_ASSERT(!peer.is_media_ready(), "media should not be ready before transport connected");

    peer.set_ice_transport_state(IceTransportState::connected);
    TEST_ASSERT(!peer.is_media_ready(), "media should not be ready before DTLS connected");
    peer.set_dtls_transport_state(DtlsTransportState::connected);
    TEST_ASSERT(peer.is_transport_ready(), "transport should be ready when ICE and DTLS connected");
    TEST_ASSERT(peer.is_media_ready(), "media should be ready when signaling stable and transport connected");

    TEST_ASSERT(peer.poll_scheduled_rtcp(300, pkt), "scheduler should activate after stable transition");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "first scheduled packet after stable should be RR without sender activity");
    return true;
}

bool test_webrtc_peer_session_signaling_json_and_sender_report_schedule()
{
    const uint32_t local_ssrc = 0x51515151;
    const uint32_t remote_ssrc = 0x52525252;

    WebrtcPeerSession peer(local_ssrc, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 2000, 0), 0), "peer RTP packet 1 should be accepted");
    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 2001, 3000), 33), "peer RTP packet 2 should be accepted");

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "remote offer JSON should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "local answer JSON should apply");

    peer.set_ice_transport_state(IceTransportState::connected);
    peer.set_dtls_transport_state(DtlsTransportState::connected);

    peer.on_sender_activity(0x2020202030303030ull, 1234, 7, 700);

    RtcpPacket pkt;
    TEST_ASSERT(peer.poll_scheduled_rtcp(300, pkt), "first scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "first scheduled packet should be SR when sender activity exists");

    TEST_ASSERT(peer.poll_scheduled_rtcp(400, pkt), "second scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "second scheduled packet should be RR for sender_report_every=2");

    TEST_ASSERT(peer.poll_scheduled_rtcp(500, pkt), "third scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "third scheduled packet should return to SR");
    return true;
}

bool test_webrtc_peer_session_transport_state_gates_scheduler()
{
    const uint32_t local_ssrc = 0x61616161;
    const uint32_t remote_ssrc = 0x62626262;

    WebrtcPeerSession peer(local_ssrc, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 3000, 0), 0), "peer RTP should be accepted");

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "remote offer JSON should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "local answer JSON should apply");

    RtcpPacket pkt;
    TEST_ASSERT(!peer.poll_scheduled_rtcp(300, pkt), "scheduler should stay blocked when transport not connected");

    peer.set_ice_transport_state(IceTransportState::checking);
    peer.set_dtls_transport_state(DtlsTransportState::connecting);
    TEST_ASSERT(!peer.is_transport_ready(), "transport should not be ready in checking/connecting");
    TEST_ASSERT(!peer.poll_scheduled_rtcp(400, pkt), "scheduler should stay blocked while transport negotiating");

    peer.set_ice_transport_state(IceTransportState::connected);
    peer.set_dtls_transport_state(DtlsTransportState::connected);
    TEST_ASSERT(peer.is_transport_ready(), "transport should become ready when both connected");
    TEST_ASSERT(peer.poll_scheduled_rtcp(500, pkt), "scheduler should emit once transport connected");
    return true;
}

bool test_webrtc_peer_session_connection_state_callback_lifecycle()
{
    WebrtcPeerSession peer(0x71717171, 90000);

    std::vector<PeerConnectionState> observed;
    peer.set_connection_state_callback([&observed](PeerConnectionState previous, PeerConnectionState current) {
        (void) previous;
        observed.push_back(current);
    });

    TEST_ASSERT(peer.connection_state() == PeerConnectionState::new_, "initial connection state should be new");

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "remote offer should apply");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connecting, "state should become connecting after negotiation starts");

    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "local answer should apply");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connecting, "state should remain connecting until transports connect");

    peer.set_ice_transport_state(IceTransportState::connected);
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connecting, "state should stay connecting until DTLS connects");

    peer.set_dtls_transport_state(DtlsTransportState::connected);
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connected, "state should become connected when ICE and DTLS connected");

    peer.set_dtls_transport_state(DtlsTransportState::failed);
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::failed, "state should become failed when DTLS fails");

    TEST_ASSERT(observed.size() == 3, "callback should record 3 transitions");
    TEST_ASSERT(observed[0] == PeerConnectionState::connecting, "first transition should be connecting");
    TEST_ASSERT(observed[1] == PeerConnectionState::connected, "second transition should be connected");
    TEST_ASSERT(observed[2] == PeerConnectionState::failed, "third transition should be failed");
    return true;
}

bool test_webrtc_peer_session_transport_state_via_signaling_messages()
{
    WebrtcPeerSession peer(0x81818181, 90000);

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "local answer should apply");

    TEST_ASSERT(!peer.is_transport_ready(), "transport should start not ready");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"ice_state\",\"state\":\"connected\"}", true, 250),
                "ice_state signaling should apply");
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::connected, "ICE state should update from signaling");
    TEST_ASSERT(!peer.is_transport_ready(), "transport should still wait for DTLS");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"dtls_state\",\"state\":\"connected\"}", true, 300),
                "dtls_state signaling should apply");
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::connected, "DTLS state should update from signaling");
    TEST_ASSERT(peer.is_transport_ready(), "transport should be ready after ICE+DTLS connected");

    RtcpPacket pkt;
    TEST_ASSERT(peer.poll_scheduled_rtcp(400, pkt), "scheduler should emit once readiness reached via signaling messages");
    return true;
}

bool test_webrtc_peer_session_mock_transport_auto_progression()
{
    WebrtcPeerSession peer(0x83838383, 90000);

    MockIceTransportConfig ice_cfg;
    ice_cfg.connect_delay_ms = 50;
    ice_cfg.fail_timeout_ms = 500;
    ice_cfg.auto_start_on_candidate = true;
    peer.set_mock_ice_transport_config(ice_cfg);

    MockDtlsTransportConfig dtls_cfg;
    dtls_cfg.handshake_delay_ms = 40;
    dtls_cfg.fail_timeout_ms = 500;
    dtls_cfg.auto_start_when_ice_connected = true;
    peer.set_mock_dtls_transport_config(dtls_cfg);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "mock transport: remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120),
                "mock transport: local answer should apply");
    TEST_ASSERT(peer.signaling_state() == SignalingState::stable, "mock transport: signaling should be stable");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "mock transport: remote candidate should apply");
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::checking, "mock transport: ICE should enter checking");
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::new_, "mock transport: DTLS should stay new before ICE connected");

    peer.advance_transport(180);
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::connected, "mock transport: ICE should connect after delay");
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::connecting,
                "mock transport: DTLS should start connecting once ICE connected");

    peer.advance_transport(220);
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::connected,
                "mock transport: DTLS should connect after handshake delay");
    TEST_ASSERT(peer.is_transport_ready(), "mock transport: transport should be ready");
    TEST_ASSERT(peer.is_media_ready(), "mock transport: media should be ready once stable+transport ready");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connected,
                "mock transport: peer connection should be connected");
    return true;
}

bool test_webrtc_peer_session_mock_transport_manual_override()
{
    WebrtcPeerSession peer(0x84848484, 90000);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "mock override: remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120),
                "mock override: local answer should apply");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"ice_state\",\"state\":\"failed\"}", true, 130),
                "mock override: ICE failed signaling should apply");
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::failed, "mock override: ICE should be failed");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::failed,
                "mock override: connection should be failed when ICE failed");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"ice_state\",\"state\":\"connected\"}", true, 140),
                "mock override: ICE connected signaling should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"dtls_state\",\"state\":\"connected\"}", true, 150),
                "mock override: DTLS connected signaling should apply");
    TEST_ASSERT(peer.is_transport_ready(), "mock override: transport should recover to ready");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connected,
                "mock override: connection should recover to connected");
    return true;
}

bool test_webrtc_peer_session_e2e_scripted_timeline()
{
    const uint32_t local_ssrc = 0x91919191;
    const uint32_t remote_ssrc = 0x92929292;

    WebrtcPeerSession peer(local_ssrc, 90000);
    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    config.rr_snapshot_reset_interval_reports = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "timeline remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "timeline local answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          220),
                "timeline candidate should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"ice_state\",\"state\":\"connected\"}", true, 250),
                "timeline ice connected should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"dtls_state\",\"state\":\"connected\"}", true, 300),
                "timeline dtls connected should apply");

    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 4000, 0), 310), "timeline RTP pkt1 should be accepted");
    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 4002, 6000), 376), "timeline RTP pkt2 should be accepted");
    TEST_ASSERT(peer.on_rtp_packet_received(make_rtp(remote_ssrc, 4001, 3000), 343), "timeline RTP pkt3 should be accepted");
    peer.on_sender_activity(0x3030303040404040ull, 888, 9, 900);

    RtcpPacket pkt;
    TEST_ASSERT(peer.poll_scheduled_rtcp(400, pkt), "timeline first scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "timeline first packet should be SR");

    TEST_ASSERT(peer.poll_scheduled_rtcp(500, pkt), "timeline second scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::receiver_report, "timeline second packet should be RR");
    TEST_ASSERT(pkt.receiver_report.report_blocks.size() == 1, "timeline RR should have one block");
    TEST_ASSERT(pkt.receiver_report.report_blocks[0].ssrc == remote_ssrc, "timeline RR block SSRC should match remote");

    TEST_ASSERT(peer.poll_scheduled_rtcp(600, pkt), "timeline third scheduled packet should emit");
    TEST_ASSERT(pkt.kind == RtcpPacket::Kind::sender_report, "timeline third packet should be SR");
    return true;
}

bool test_webrtc_peer_session_snapshot_reflects_runtime_state()
{
    const uint32_t local_ssrc = 0xA1A1A1A1;

    WebrtcPeerSession peer(local_ssrc, 90000);
    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    auto snap = peer.snapshot();
    TEST_ASSERT(snap.signaling_state == SignalingState::new_, "snapshot signaling should start at new");
    TEST_ASSERT(snap.connection_state == PeerConnectionState::new_, "snapshot connection should start at new");
    TEST_ASSERT(!snap.media_ready, "snapshot media_ready should start false");
    TEST_ASSERT(!snap.scheduler_active, "snapshot scheduler should start inactive");

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100),
                "snapshot remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 200),
                "snapshot local answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          210),
                "snapshot candidate should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"ice_state\",\"state\":\"connected\"}", true, 250),
                "snapshot ice connected should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"dtls_state\",\"state\":\"connected\"}", true, 300),
                "snapshot dtls connected should apply");

    snap = peer.snapshot();
    TEST_ASSERT(snap.signaling_state == SignalingState::stable, "snapshot signaling should be stable");
    TEST_ASSERT(snap.ice_transport_state == IceTransportState::connected, "snapshot ICE should be connected");
    TEST_ASSERT(snap.dtls_transport_state == DtlsTransportState::connected, "snapshot DTLS should be connected");
    TEST_ASSERT(snap.connection_state == PeerConnectionState::connected, "snapshot connection should be connected");
    TEST_ASSERT(snap.has_remote_description, "snapshot should have remote description");
    TEST_ASSERT(snap.has_local_description, "snapshot should have local description");
    TEST_ASSERT(snap.remote_candidate_count == 1, "snapshot candidate count should be 1");
    TEST_ASSERT(snap.transport_ready, "snapshot transport_ready should be true");
    TEST_ASSERT(snap.media_ready, "snapshot media_ready should be true");

    RtcpPacket pkt;
    TEST_ASSERT(peer.poll_scheduled_rtcp(400, pkt), "snapshot should observe scheduler active through emitted packet");
    snap = peer.snapshot();
    TEST_ASSERT(snap.scheduler_active, "snapshot scheduler should be active after media ready");

    const std::string snap_json = peer.snapshot_json();
    WebrtcPeerSessionSnapshot parsed;
    TEST_ASSERT(peer.parse_snapshot_json(snap_json, parsed), "snapshot JSON should parse");
    TEST_ASSERT(parsed.signaling_state == snap.signaling_state, "parsed snapshot signaling_state should match");
    TEST_ASSERT(parsed.ice_transport_state == snap.ice_transport_state, "parsed snapshot ICE state should match");
    TEST_ASSERT(parsed.dtls_transport_state == snap.dtls_transport_state, "parsed snapshot DTLS state should match");
    TEST_ASSERT(parsed.connection_state == snap.connection_state, "parsed snapshot connection state should match");
    TEST_ASSERT(parsed.has_local_description == snap.has_local_description, "parsed snapshot local description flag should match");
    TEST_ASSERT(parsed.has_remote_description == snap.has_remote_description, "parsed snapshot remote description flag should match");
    TEST_ASSERT(parsed.remote_candidate_count == snap.remote_candidate_count, "parsed snapshot candidate count should match");
    TEST_ASSERT(parsed.transport_ready == snap.transport_ready, "parsed snapshot transport_ready should match");
    TEST_ASSERT(parsed.media_ready == snap.media_ready, "parsed snapshot media_ready should match");
    TEST_ASSERT(parsed.scheduler_active == snap.scheduler_active, "parsed snapshot scheduler_active should match");
    TEST_ASSERT(parsed.dtls_fingerprint_consistent == snap.dtls_fingerprint_consistent,
                "parsed snapshot fingerprint consistency should match");
    TEST_ASSERT(parsed.dtls_fingerprint_policy_required == snap.dtls_fingerprint_policy_required,
                "parsed snapshot fingerprint policy flag should match");
    TEST_ASSERT(parsed.security_error == snap.security_error,
                "parsed snapshot security_error should match");
    TEST_ASSERT(parsed.security_error_code == snap.security_error_code,
                "parsed snapshot security_error_code should match");
    TEST_ASSERT(parsed.selected_ice_pair_nomination_transaction_id == snap.selected_ice_pair_nomination_transaction_id,
                "parsed snapshot selected_ice_pair_nomination_transaction_id should match");
    TEST_ASSERT(parsed.stun_transactions.size() == snap.stun_transactions.size(),
                "parsed snapshot stun_transactions size should match");
    TEST_ASSERT(parsed.pending_ice_nomination_signal_count == snap.pending_ice_nomination_signal_count,
                "parsed snapshot pending_ice_nomination_signal_count should match");
    TEST_ASSERT(parsed.peak_pending_ice_nomination_signal_count == snap.peak_pending_ice_nomination_signal_count,
                "parsed snapshot peak_pending_ice_nomination_signal_count should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_count == snap.dropped_ice_nomination_signal_count,
                "parsed snapshot dropped_ice_nomination_signal_count should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_overflow_count == snap.dropped_ice_nomination_signal_overflow_count,
                "parsed snapshot dropped_ice_nomination_signal_overflow_count should match");
    TEST_ASSERT(parsed.dropped_ice_nomination_signal_trim_count == snap.dropped_ice_nomination_signal_trim_count,
                "parsed snapshot dropped_ice_nomination_signal_trim_count should match");
    TEST_ASSERT(parsed.pending_diagnostics_signal_count == snap.pending_diagnostics_signal_count,
                "parsed snapshot pending_diagnostics_signal_count should match");
    TEST_ASSERT(parsed.peak_pending_diagnostics_signal_count == snap.peak_pending_diagnostics_signal_count,
                "parsed snapshot peak_pending_diagnostics_signal_count should match");
    TEST_ASSERT(parsed.dropped_diagnostics_signal_count == snap.dropped_diagnostics_signal_count,
                "parsed snapshot dropped_diagnostics_signal_count should match");
    TEST_ASSERT(parsed.dropped_diagnostics_signal_overflow_count == snap.dropped_diagnostics_signal_overflow_count,
                "parsed snapshot dropped_diagnostics_signal_overflow_count should match");
    TEST_ASSERT(parsed.dropped_diagnostics_signal_trim_count == snap.dropped_diagnostics_signal_trim_count,
                "parsed snapshot dropped_diagnostics_signal_trim_count should match");
    TEST_ASSERT(parsed.diagnostics_v2_flat_duplicate_seen_count == snap.diagnostics_v2_flat_duplicate_seen_count,
                "parsed snapshot diagnostics_v2_flat_duplicate_seen_count should match");
    TEST_ASSERT(parsed.diagnostics_v2_flat_duplicate_mismatch_count == snap.diagnostics_v2_flat_duplicate_mismatch_count,
                "parsed snapshot diagnostics_v2_flat_duplicate_mismatch_count should match");
    TEST_ASSERT(parsed.diagnostics_emit_flat_compat_fields == snap.diagnostics_emit_flat_compat_fields,
                "parsed snapshot diagnostics_emit_flat_compat_fields should match");
    TEST_ASSERT(parsed.diagnostics_release_mode_strict_v2 == snap.diagnostics_release_mode_strict_v2,
                "parsed snapshot diagnostics_release_mode_strict_v2 should match");
    TEST_ASSERT(parsed.diagnostics_rollout_alert_window_seconds == snap.diagnostics_rollout_alert_window_seconds,
                "parsed snapshot diagnostics_rollout_alert_window_seconds should match");
    TEST_ASSERT(parsed.diagnostics_rollout_mismatch_count_alert_threshold
                    == snap.diagnostics_rollout_mismatch_count_alert_threshold,
                "parsed snapshot diagnostics_rollout_mismatch_count_alert_threshold should match");
    TEST_ASSERT(parsed.diagnostics_rollout_mismatch_ratio_threshold_ppm
                    == snap.diagnostics_rollout_mismatch_ratio_threshold_ppm,
                "parsed snapshot diagnostics_rollout_mismatch_ratio_threshold_ppm should match");
    TEST_ASSERT(parsed.diagnostics_rollout_current_mismatch_ratio_ppm
                    == snap.diagnostics_rollout_current_mismatch_ratio_ppm,
                "parsed snapshot diagnostics_rollout_current_mismatch_ratio_ppm should match");
    TEST_ASSERT(parsed.diagnostics_rollout_alert_active
                    == snap.diagnostics_rollout_alert_active,
                "parsed snapshot diagnostics_rollout_alert_active should match");
    TEST_ASSERT(parsed.diagnostics_rollout_progress_blocked
                    == snap.diagnostics_rollout_progress_blocked,
                "parsed snapshot diagnostics_rollout_progress_blocked should match");
    TEST_ASSERT(parsed.diagnostics_rollout_ready_for_progress
                    == snap.diagnostics_rollout_ready_for_progress,
                "parsed snapshot diagnostics_rollout_ready_for_progress should match");
    TEST_ASSERT(parsed.diagnostics_policy_keep_latest_only == snap.diagnostics_policy_keep_latest_only,
                "parsed snapshot diagnostics_policy_keep_latest_only should match");
    TEST_ASSERT(parsed.diagnostics_policy_max_pending_signals == snap.diagnostics_policy_max_pending_signals,
                "parsed snapshot diagnostics_policy_max_pending_signals should match");
    TEST_ASSERT(parsed.diagnostics_policy_nomination_max_pending_signals
                    == snap.diagnostics_policy_nomination_max_pending_signals,
                "parsed snapshot diagnostics_policy_nomination_max_pending_signals should match");
    TEST_ASSERT(parsed.has_last_stun_transaction == snap.has_last_stun_transaction,
                "parsed snapshot has_last_stun_transaction should match");
    TEST_ASSERT(parsed.last_stun_transaction.transaction_id == snap.last_stun_transaction.transaction_id,
                "parsed snapshot last_stun_transaction id should match");

    TEST_ASSERT(!peer.parse_snapshot_json("{\"invalid\":true}", parsed), "invalid snapshot JSON should fail parsing");
    return true;
}

bool test_webrtc_peer_session_srtp_end_to_end_hooks()
{
    const uint32_t local_ssrc = 0xA2A2A2A2;
    const uint32_t remote_ssrc = 0xB2B2B2B2;

    WebrtcPeerSession peer(local_ssrc, 90000);
    MockSrtpContext *raw_ctx = new MockSrtpContext();
    std::shared_ptr<SrtpContext> ctx(raw_ctx);
    peer.set_srtp_context(ctx);
    TEST_ASSERT(peer.has_srtp_context(), "peer should report SRTP context installed");

    RtcpScheduleConfig config;
    config.report_interval_ms = 100;
    config.sender_report_every = 2;
    peer.set_rtcp_schedule_config(config);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100), "srtp peer: remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "srtp peer: local answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "srtp peer: candidate should apply");
    peer.advance_transport(220);
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::connected,
                "srtp peer: ICE should be connected after first transport advance");
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::connecting,
                "srtp peer: DTLS should be connecting after ICE connected");
    peer.advance_transport(300);
    TEST_ASSERT(peer.is_media_ready(), "srtp peer: media should be ready after transport progression");

    RtcPacket rtp = make_rtp(remote_ssrc, 5000, 0);
    TEST_ASSERT(peer.on_rtp_packet_received(rtp, 230), "srtp peer: RTP receive should pass");
    TEST_ASSERT(raw_ctx->unprotect_rtp_calls() == 1, "srtp peer: unprotect_rtp should be called");

    peer.on_sender_activity(0x3031323334353637ull, 777, 3, 333);
    RtcpPacket pkt;
    TEST_ASSERT(peer.poll_scheduled_rtcp(400, pkt), "srtp peer: scheduled RTCP should emit");
    TEST_ASSERT(raw_ctx->protect_rtcp_calls() == 1, "srtp peer: protect_rtcp should be called");
    return true;
}

class InstantIceTransport final : public IceTransportEngine
{
public:
    void reset() override
    {
        state_ = IceTransportState::new_;
        remote_count_ = 0;
    }

    void set_state(IceTransportState state) override
    {
        state_ = state;
    }

    IceTransportState state() const override
    {
        return state_;
    }

    void on_remote_candidate(const IceCandidate &candidate, uint64_t now_ms) override
    {
        (void) now_ms;
        if (!candidate.candidate.empty()) {
            ++remote_count_;
            state_ = IceTransportState::connected;
        }
    }

    void start_checking(uint64_t now_ms) override
    {
        (void) now_ms;
        state_ = IceTransportState::checking;
    }

    void poll(uint64_t now_ms) override
    {
        (void) now_ms;
    }

    std::size_t remote_candidate_count() const override
    {
        return remote_count_;
    }

    void set_pair_selector(std::shared_ptr<IcePairSelector> selector) override
    {
        (void) selector;
    }

private:
    IceTransportState state_ = IceTransportState::new_;
    std::size_t remote_count_ = 0;
};

class InstantDtlsTransport final : public DtlsTransportEngine
{
public:
    void reset() override
    {
        state_ = DtlsTransportState::new_;
    }

    void set_state(DtlsTransportState state) override
    {
        state_ = state;
    }

    DtlsTransportState state() const override
    {
        return state_;
    }

    void on_ice_state_changed(IceTransportState ice_state, uint64_t now_ms) override
    {
        (void) now_ms;
        if (ice_state == IceTransportState::connected) {
            state_ = DtlsTransportState::connected;
        } else if (ice_state == IceTransportState::failed) {
            state_ = DtlsTransportState::failed;
        }
    }

    void start_handshake(uint64_t now_ms) override
    {
        (void) now_ms;
        state_ = DtlsTransportState::connected;
    }

    void poll(uint64_t now_ms, IceTransportState ice_state) override
    {
        (void) now_ms;
        if (ice_state == IceTransportState::connected && state_ == DtlsTransportState::new_) {
            state_ = DtlsTransportState::connected;
        }
    }

    DtlsFingerprintVerificationState fingerprint_verification_state() const override
    {
        return state_ == DtlsTransportState::connected
                   ? DtlsFingerprintVerificationState::verified
                   : DtlsFingerprintVerificationState::unknown;
    }

    DtlsPeerFingerprint peer_fingerprint() const override
    {
        DtlsPeerFingerprint out;
        if (state_ == DtlsTransportState::connected) {
            out.algorithm = "sha-256";
            out.value = "11:22:33";
        }
        return out;
    }

private:
    DtlsTransportState state_ = DtlsTransportState::new_;
};

bool test_webrtc_peer_session_pluggable_transport_engines()
{
    WebrtcPeerSession peer(0xC3C3C3C3, 90000);
    peer.set_ice_transport_engine(std::make_shared<InstantIceTransport>());
    peer.set_dtls_transport_engine(std::make_shared<InstantDtlsTransport>());

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100), "pluggable: remote offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "pluggable: local answer should apply");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "pluggable: candidate should apply");
    TEST_ASSERT(peer.ice_transport_state() == IceTransportState::connected, "pluggable: ICE should connect instantly");
    TEST_ASSERT(peer.dtls_transport_state() == DtlsTransportState::connected, "pluggable: DTLS should connect instantly");
    TEST_ASSERT(peer.is_media_ready(), "pluggable: media should be ready");
    TEST_ASSERT(peer.connection_state() == PeerConnectionState::connected, "pluggable: peer should be connected");
    return true;
}

bool test_webrtc_peer_session_ice_candidate_observability()
{
    WebrtcPeerSession peer(0xD4D4D4D4, 90000);

    std::vector<IceCandidate> local;
    IceCandidate l1;
    l1.candidate = "candidate:10 1 UDP 2122260223 192.168.0.2 5000 typ host";
    l1.mid = "0";
    l1.mline_index = 0;
    local.push_back(l1);
    peer.set_local_ice_candidates(local);

    TEST_ASSERT(peer.local_ice_candidates().size() == 1, "ice obs: local candidate should be stored");
    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100), "ice obs: offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "ice obs: answer should apply");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "ice obs: remote candidate should apply");

    peer.advance_transport(300);
    TEST_ASSERT(peer.remote_ice_candidates().size() == 1, "ice obs: remote candidate should be tracked");
    TEST_ASSERT(peer.has_selected_ice_pair(), "ice obs: selected pair should be available once connected");
    const IceCandidatePair pair = peer.selected_ice_pair();
    TEST_ASSERT(pair.local.ip == "192.168.0.2", "ice obs: selected local ip should match");
    TEST_ASSERT(pair.remote.ip == "10.0.0.2", "ice obs: selected remote ip should match");
    TEST_ASSERT(pair.remote.port == 5000, "ice obs: selected remote port should match");
    return true;
}

bool test_webrtc_peer_session_dtls_keying_material_propagates_to_srtp()
{
    WebrtcPeerSession peer(0xE5E5E5E5, 90000);
    MockSrtpContext *raw_ctx = new MockSrtpContext();
    std::shared_ptr<SrtpContext> ctx(raw_ctx);
    peer.set_srtp_context(ctx);

    TEST_ASSERT(peer.apply_signaling_json(valid_offer_json(), true, 100), "dtls key: offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "dtls key: answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "dtls key: candidate should apply");

    peer.advance_transport(300);
    peer.advance_transport(400);
    TEST_ASSERT(peer.has_dtls_srtp_keying_material(), "dtls key: peer should expose keying material");
    const DtlsSrtpKeyingMaterial material = peer.dtls_srtp_keying_material();
    TEST_ASSERT(material.profile == DtlsSrtpProfile::srtp_aes128_cm_sha1_80,
                "dtls key: SRTP profile should match mock profile");
    TEST_ASSERT(material.client_master_key.size() == 16, "dtls key: client key size should be 16");
    TEST_ASSERT(material.server_master_key.size() == 16, "dtls key: server key size should be 16");
    TEST_ASSERT(material.client_master_salt.size() == 14, "dtls key: client salt size should be 14");
    TEST_ASSERT(material.server_master_salt.size() == 14, "dtls key: server salt size should be 14");
    TEST_ASSERT(raw_ctx->has_keying_material(), "dtls key: SRTP context should receive keying material");
    TEST_ASSERT(peer.dtls_fingerprint_verification_state() == DtlsFingerprintVerificationState::verified,
                "dtls key: fingerprint verification should be verified");
    const DtlsPeerFingerprint fingerprint = peer.dtls_peer_fingerprint();
    TEST_ASSERT(fingerprint.algorithm == "sha-256", "dtls key: fingerprint algorithm should be set");
    TEST_ASSERT(!fingerprint.value.empty(), "dtls key: fingerprint value should be set");
    return true;
}

bool test_webrtc_peer_session_fingerprint_consistency_with_remote_sdp()
{
    WebrtcPeerSession peer(0xF6F6F6F6, 90000);
    MockSrtpContext *raw_ctx = new MockSrtpContext();
    std::shared_ptr<SrtpContext> ctx(raw_ctx);
    peer.set_srtp_context(ctx);

    const std::string offer_with_fp =
        "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 1 IN IP4 127.0.0.1\\r\\ns=-\\r\\nt=0 0\\r\\na=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\\r\\na=group:BUNDLE 0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\na=mid:0\\r\\na=sendrecv\\r\\na=rtpmap:111 opus/48000/2\\r\\n\"}";

    TEST_ASSERT(peer.apply_signaling_json(offer_with_fp, true, 100), "fp match: offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "fp match: answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "fp match: candidate should apply");

    peer.advance_transport(400);
    peer.advance_transport(500);
    peer.advance_transport(600);
    TEST_ASSERT(peer.dtls_fingerprint_verification_state() == DtlsFingerprintVerificationState::verified,
                "fp match: dtls fingerprint should be verified");
    TEST_ASSERT(peer.is_dtls_fingerprint_consistent_with_remote_sdp(),
                "fp match: dtls fingerprint should match remote SDP fingerprint");

    auto snap = peer.snapshot();
    TEST_ASSERT(snap.dtls_fingerprint_consistent, "fp match: snapshot should reflect fingerprint consistency");
    return true;
}

bool test_webrtc_peer_session_fingerprint_policy_blocks_media_when_mismatch()
{
    WebrtcPeerSession peer(0xF7F7F7F7, 90000);
    peer.set_require_dtls_fingerprint_match_for_media(true);

    const std::string mismatched_offer =
        "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\no=- 1 1 IN IP4 127.0.0.1\\r\\ns=-\\r\\nt=0 0\\r\\na=fingerprint:sha-256 FF:EE:DD:CC\\r\\na=group:BUNDLE 0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\na=mid:0\\r\\na=sendrecv\\r\\na=rtpmap:111 opus/48000/2\\r\\n\"}";

    TEST_ASSERT(peer.apply_signaling_json(mismatched_offer, true, 100), "fp policy: offer should apply");
    TEST_ASSERT(peer.apply_signaling_json(valid_answer_json(), false, 120), "fp policy: answer should apply");
    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"candidate\",\"candidate\":\"candidate:1 1 UDP 2122260223 10.0.0.2 5000 typ host\",\"mid\":\"0\",\"mline_index\":0}",
                                          true,
                                          130),
                "fp policy: candidate should apply");

    peer.advance_transport(500);
    peer.advance_transport(600);
    TEST_ASSERT(peer.is_transport_ready(), "fp policy: transport should be ready");
    TEST_ASSERT(!peer.is_dtls_fingerprint_consistent_with_remote_sdp(),
                "fp policy: fingerprint should be inconsistent");
    TEST_ASSERT(!peer.is_media_ready(), "fp policy: media should be blocked on mismatch");
    TEST_ASSERT(peer.last_security_error() == "dtls_fingerprint_mismatch",
                "fp policy: security error should report mismatch");

    const auto snap = peer.snapshot();
    TEST_ASSERT(snap.dtls_fingerprint_policy_required,
                "fp policy: snapshot should indicate policy required");
    TEST_ASSERT(snap.security_error == "dtls_fingerprint_mismatch",
                "fp policy: snapshot should contain mismatch error");

    TEST_ASSERT(peer.apply_signaling_json("{\"type\":\"security_state\",\"security_error\":\"forced_security_error\"}",
                                          true,
                                          700),
                "fp policy: security_state signaling should apply");
    TEST_ASSERT(peer.last_security_error() == "forced_security_error",
                "fp policy: security_state signaling should override security error");
    TEST_ASSERT(peer.last_security_error_code() == SecurityErrorCode::external_security_error,
                "fp policy: security_state signaling should set external security error code");
    return true;
}

} // namespace

int main()
{
    std::cout << "=== WebRTC Bridge Test Suite ===\n";
    RUN_TEST(test_webrtc_signaling_bridge_basics);
    RUN_TEST(test_webrtc_signaling_bridge_strict_sdp_validation_and_error_codes);
    RUN_TEST(test_webrtc_signaling_bridge_state_transitions);
    RUN_TEST(test_webrtc_signaling_json_roundtrip_and_apply);
    RUN_TEST(test_webrtc_signaling_bridge_negotiation_validation_errors);
    RUN_TEST(test_webrtc_candidate_parser_and_ice_scaffold_lifecycle);
    RUN_TEST(test_webrtc_peer_session_provider_backed_ice_failure_path);
    RUN_TEST(test_webrtc_peer_session_provider_stun_timeout_and_nomination_signal);
    RUN_TEST(test_webrtc_peer_session_provider_parallel_stun_transactions_and_snapshot_roundtrip);
    RUN_TEST(test_webrtc_peer_session_nomination_signal_polling_and_duplicate_suppression);
    RUN_TEST(test_webrtc_peer_session_nomination_signal_polling_queue_order);
    RUN_TEST(test_webrtc_peer_session_nomination_signal_queue_capacity_and_drop_counter);
    RUN_TEST(test_webrtc_peer_session_nomination_signal_queue_config_clamps_and_trims);
    RUN_TEST(test_webrtc_peer_session_signal_queue_runtime_config_unified_surface);
    RUN_TEST(test_webrtc_peer_session_diagnostics_signal_reflects_runtime);
    RUN_TEST(test_webrtc_peer_session_diagnostics_queue_mode_transition_matrix);
    RUN_TEST(test_webrtc_transport_bridge_rtp_rtcp_pipeline);
    RUN_TEST(test_webrtc_transport_bridge_rtcp_scheduler_deterministic_tick);
    RUN_TEST(test_webrtc_transport_bridge_schedule_snapshot_reset_effect);
    RUN_TEST(test_webrtc_transport_bridge_srtp_hooks_and_failures);
    RUN_TEST(test_webrtc_peer_session_scheduler_activation_by_signaling_state);
    RUN_TEST(test_webrtc_peer_session_signaling_json_and_sender_report_schedule);
    RUN_TEST(test_webrtc_peer_session_transport_state_gates_scheduler);
    RUN_TEST(test_webrtc_peer_session_connection_state_callback_lifecycle);
    RUN_TEST(test_webrtc_peer_session_transport_state_via_signaling_messages);
    RUN_TEST(test_webrtc_peer_session_mock_transport_auto_progression);
    RUN_TEST(test_webrtc_peer_session_mock_transport_manual_override);
    RUN_TEST(test_webrtc_peer_session_e2e_scripted_timeline);
    RUN_TEST(test_webrtc_peer_session_snapshot_reflects_runtime_state);
    RUN_TEST(test_webrtc_peer_session_srtp_end_to_end_hooks);
    RUN_TEST(test_webrtc_peer_session_pluggable_transport_engines);
    RUN_TEST(test_webrtc_peer_session_ice_candidate_observability);
    RUN_TEST(test_webrtc_peer_session_dtls_keying_material_propagates_to_srtp);
    RUN_TEST(test_webrtc_peer_session_fingerprint_consistency_with_remote_sdp);
    RUN_TEST(test_webrtc_peer_session_fingerprint_policy_blocks_media_when_mismatch);

    std::cout << "Result: " << g_pass << "/" << g_run << " passed";
    if (g_fail > 0) {
        std::cout << ", " << g_fail << " failed";
    }
    std::cout << "\n";
    return g_fail == 0 ? 0 : 1;
}
