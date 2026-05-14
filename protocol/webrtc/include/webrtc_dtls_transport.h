#ifndef __NET_WEBRTC_DTLS_TRANSPORT_H__
#define __NET_WEBRTC_DTLS_TRANSPORT_H__

#include "webrtc_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::webrtc
{

enum class DtlsSrtpProfile
{
    unknown,
    srtp_aes128_cm_sha1_80,
};

struct DtlsSrtpKeyingMaterial
{
    DtlsSrtpProfile profile = DtlsSrtpProfile::unknown;
    std::vector<uint8_t> client_master_key;
    std::vector<uint8_t> server_master_key;
    std::vector<uint8_t> client_master_salt;
    std::vector<uint8_t> server_master_salt;
};

enum class DtlsFingerprintVerificationState
{
    unknown,
    verified,
    failed,
};

struct DtlsPeerFingerprint
{
    std::string algorithm;
    std::string value;
};

class DtlsTransportEngine
{
public:
    virtual ~DtlsTransportEngine() = default;

    virtual void reset() = 0;
    virtual void set_state(DtlsTransportState state) = 0;
    virtual DtlsTransportState state() const = 0;
    virtual void on_ice_state_changed(IceTransportState ice_state, uint64_t now_ms) = 0;
    virtual void start_handshake(uint64_t now_ms) = 0;
    virtual void poll(uint64_t now_ms, IceTransportState ice_state) = 0;
    virtual bool has_srtp_keying_material() const
    {
        return false;
    }
    virtual DtlsSrtpKeyingMaterial srtp_keying_material() const
    {
        return {};
    }
    virtual DtlsFingerprintVerificationState fingerprint_verification_state() const
    {
        return DtlsFingerprintVerificationState::unknown;
    }
    virtual DtlsPeerFingerprint peer_fingerprint() const
    {
        return {};
    }
};

struct MockDtlsTransportConfig
{
    uint64_t handshake_delay_ms = 60;
    uint64_t fail_timeout_ms = 1500;
    bool auto_start_when_ice_connected = true;
};

class MockDtlsTransport : public DtlsTransportEngine
{
public:
    void set_config(const MockDtlsTransportConfig &config);
    MockDtlsTransportConfig config() const;

    void reset() override;
    void set_state(DtlsTransportState state) override;
    DtlsTransportState state() const override;

    void on_ice_state_changed(IceTransportState ice_state, uint64_t now_ms) override;
    void start_handshake(uint64_t now_ms) override;
    void poll(uint64_t now_ms, IceTransportState ice_state) override;
    bool has_srtp_keying_material() const override;
    DtlsSrtpKeyingMaterial srtp_keying_material() const override;
    DtlsFingerprintVerificationState fingerprint_verification_state() const override;
    DtlsPeerFingerprint peer_fingerprint() const override;

private:
    void ensure_keying_material_generated();

    MockDtlsTransportConfig config_;
    DtlsTransportState state_ = DtlsTransportState::new_;
    uint64_t handshake_started_at_ms_ = 0;
    bool has_srtp_keying_material_ = false;
    DtlsSrtpKeyingMaterial srtp_keying_material_;
    DtlsFingerprintVerificationState fingerprint_verification_state_ = DtlsFingerprintVerificationState::unknown;
    DtlsPeerFingerprint peer_fingerprint_;
};

} // namespace yuan::net::webrtc

#endif
