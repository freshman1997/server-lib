#include "shadowsocks_protocol.h"

namespace yuan::net::shadowsocks
{
    namespace
    {
        constexpr MethodSpec kAes128GcmSpec{
            CipherMethod::aes_128_gcm,
            "aes-128-gcm",
            16,
            16,
            12,
            16
        };

        constexpr MethodSpec kAes256GcmSpec{
            CipherMethod::aes_256_gcm,
            "aes-256-gcm",
            32,
            32,
            12,
            16
        };

        constexpr MethodSpec kChaCha20IetfPoly1305Spec{
            CipherMethod::chacha20_ietf_poly1305,
            "chacha20-ietf-poly1305",
            32,
            32,
            12,
            16
        };
    }

    std::optional<CipherMethod> parse_method(std::string_view method_name)
    {
        if (method_name == kAes128GcmSpec.name) {
            return CipherMethod::aes_128_gcm;
        }
        if (method_name == kAes256GcmSpec.name) {
            return CipherMethod::aes_256_gcm;
        }
        if (method_name == kChaCha20IetfPoly1305Spec.name) {
            return CipherMethod::chacha20_ietf_poly1305;
        }
        return std::nullopt;
    }

    const MethodSpec &method_spec(CipherMethod method)
    {
        switch (method) {
        case CipherMethod::aes_128_gcm:
            return kAes128GcmSpec;
        case CipherMethod::aes_256_gcm:
            return kAes256GcmSpec;
        case CipherMethod::chacha20_ietf_poly1305:
            return kChaCha20IetfPoly1305Spec;
        }

        return kChaCha20IetfPoly1305Spec;
    }

    bool is_supported_method(std::string_view method_name)
    {
        return parse_method(method_name).has_value();
    }
}
