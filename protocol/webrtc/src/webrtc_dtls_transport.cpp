#include "webrtc_dtls_transport.h"

namespace yuan::net::webrtc
{

void MockDtlsTransport::set_config(const MockDtlsTransportConfig &config)
{
    config_ = config;
    if (config_.handshake_delay_ms == 0) {
        config_.handshake_delay_ms = 1;
    }
}

MockDtlsTransportConfig MockDtlsTransport::config() const
{
    return config_;
}

void MockDtlsTransport::reset()
{
    state_ = DtlsTransportState::new_;
    handshake_started_at_ms_ = 0;
    has_srtp_keying_material_ = false;
    srtp_keying_material_ = {};
    fingerprint_verification_state_ = DtlsFingerprintVerificationState::unknown;
    peer_fingerprint_ = {};
}

void MockDtlsTransport::set_state(DtlsTransportState state)
{
    state_ = state;
    if (state_ != DtlsTransportState::connected) {
        has_srtp_keying_material_ = false;
        srtp_keying_material_ = {};
        fingerprint_verification_state_ = DtlsFingerprintVerificationState::unknown;
        peer_fingerprint_ = {};
    } else {
        ensure_keying_material_generated();
        fingerprint_verification_state_ = DtlsFingerprintVerificationState::verified;
        peer_fingerprint_.algorithm = "sha-256";
        peer_fingerprint_.value = "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    }
}

DtlsTransportState MockDtlsTransport::state() const
{
    return state_;
}

void MockDtlsTransport::on_ice_state_changed(IceTransportState ice_state, uint64_t now_ms)
{
    if (ice_state == IceTransportState::failed) {
        state_ = DtlsTransportState::failed;
        fingerprint_verification_state_ = DtlsFingerprintVerificationState::failed;
        return;
    }
    if (config_.auto_start_when_ice_connected && ice_state == IceTransportState::connected && state_ == DtlsTransportState::new_) {
        start_handshake(now_ms);
    }
}

void MockDtlsTransport::start_handshake(uint64_t now_ms)
{
    if (state_ == DtlsTransportState::connected || state_ == DtlsTransportState::failed) {
        return;
    }
    state_ = DtlsTransportState::connecting;
    handshake_started_at_ms_ = now_ms;
}

void MockDtlsTransport::poll(uint64_t now_ms, IceTransportState ice_state)
{
    if (ice_state == IceTransportState::failed) {
        state_ = DtlsTransportState::failed;
        fingerprint_verification_state_ = DtlsFingerprintVerificationState::failed;
        return;
    }
    if (ice_state != IceTransportState::connected) {
        if (state_ == DtlsTransportState::connecting && now_ms >= handshake_started_at_ms_ + config_.fail_timeout_ms) {
            state_ = DtlsTransportState::failed;
        }
        return;
    }

    if (state_ != DtlsTransportState::connecting) {
        return;
    }

    const uint64_t elapsed = now_ms >= handshake_started_at_ms_ ? (now_ms - handshake_started_at_ms_) : 0;
    if (elapsed >= config_.handshake_delay_ms) {
        state_ = DtlsTransportState::connected;
        ensure_keying_material_generated();
        fingerprint_verification_state_ = DtlsFingerprintVerificationState::verified;
        peer_fingerprint_.algorithm = "sha-256";
        peer_fingerprint_.value = "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    }
}

bool MockDtlsTransport::has_srtp_keying_material() const
{
    return has_srtp_keying_material_;
}

DtlsSrtpKeyingMaterial MockDtlsTransport::srtp_keying_material() const
{
    if (!has_srtp_keying_material_) {
        return {};
    }
    return srtp_keying_material_;
}

DtlsFingerprintVerificationState MockDtlsTransport::fingerprint_verification_state() const
{
    return fingerprint_verification_state_;
}

DtlsPeerFingerprint MockDtlsTransport::peer_fingerprint() const
{
    return peer_fingerprint_;
}

void MockDtlsTransport::ensure_keying_material_generated()
{
    if (has_srtp_keying_material_) {
        return;
    }

    srtp_keying_material_.profile = DtlsSrtpProfile::srtp_aes128_cm_sha1_80;
    srtp_keying_material_.client_master_key.assign(16, 0x11);
    srtp_keying_material_.server_master_key.assign(16, 0x22);
    srtp_keying_material_.client_master_salt.assign(14, 0x33);
    srtp_keying_material_.server_master_salt.assign(14, 0x44);
    has_srtp_keying_material_ = true;
}

} // namespace yuan::net::webrtc
