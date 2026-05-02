#include "algorithm/ssh_algorithm_registry.h"
#include <algorithm>
#include <sstream>
#include <cstring>

namespace yuan::net::ssh
{
    std::unique_ptr<SshKexAlgorithm> create_kex_curve25519();
    std::unique_ptr<SshKexAlgorithm> create_kex_curve25519_libssh();
    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp256();
    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp384();
    std::unique_ptr<SshKexAlgorithm> create_kex_ecdh_nistp521();
    std::unique_ptr<SshKexAlgorithm> create_kex_dh_group14();
    std::unique_ptr<SshKexAlgorithm> create_kex_dh_group16();
    std::unique_ptr<SshKexAlgorithm> create_kex_dh_group18();

    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ed25519();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp384();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_ecdsa_nistp521();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha512();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa_sha256();
    std::unique_ptr<SshHostKeyAlgorithm> create_host_key_rsa();

    std::unique_ptr<SshCipher> create_cipher_chacha20_poly1305();
    std::unique_ptr<SshCipher> create_cipher_aes128_gcm();
    std::unique_ptr<SshCipher> create_cipher_aes256_gcm();
    std::unique_ptr<SshCipher> create_cipher_aes128_ctr();
    std::unique_ptr<SshCipher> create_cipher_aes192_ctr();
    std::unique_ptr<SshCipher> create_cipher_aes256_ctr();

    std::unique_ptr<SshMac> create_mac_hmac_sha2_256();
    std::unique_ptr<SshMac> create_mac_hmac_sha2_512();
    std::unique_ptr<SshMac> create_mac_hmac_sha1();

#if YUAN_SSH_HAS_ZLIB
    std::unique_ptr<SshCompression> create_compression_zlib();
    std::unique_ptr<SshCompression> create_compression_zlib_openssh();
#endif

    void SshAlgorithmRegistry::register_kex(const std::string & name, KexFactory factory)
    {
        kex_factories_[name] = std::move(factory);
    }

    void SshAlgorithmRegistry::register_host_key(const std::string & name, HostKeyFactory factory)
    {
        host_key_factories_[name] = std::move(factory);
    }

    void SshAlgorithmRegistry::register_cipher(const std::string & name, CipherFactory factory)
    {
        cipher_factories_[name] = std::move(factory);
    }

    void SshAlgorithmRegistry::register_mac(const std::string & name, MacFactory factory)
    {
        mac_factories_[name] = std::move(factory);
    }

    void SshAlgorithmRegistry::register_compression(const std::string & name, CompressionFactory factory)
    {
        compression_factories_[name] = std::move(factory);
    }

    std::unique_ptr<SshKexAlgorithm> SshAlgorithmRegistry::create_kex(const std::string & name) const
    {
        auto it = kex_factories_.find(name);
        if (it == kex_factories_.end())
            return nullptr;
        return it->second();
    }

    std::unique_ptr<SshHostKeyAlgorithm> SshAlgorithmRegistry::create_host_key(const std::string & name) const
    {
        auto it = host_key_factories_.find(name);
        if (it == host_key_factories_.end())
            return nullptr;
        return it->second();
    }

    std::unique_ptr<SshCipher> SshAlgorithmRegistry::create_cipher(const std::string & name) const
    {
        auto it = cipher_factories_.find(name);
        if (it == cipher_factories_.end())
            return nullptr;
        return it->second();
    }

    std::unique_ptr<SshMac> SshAlgorithmRegistry::create_mac(const std::string & name) const
    {
        auto it = mac_factories_.find(name);
        if (it == mac_factories_.end())
            return nullptr;
        return it->second();
    }

    std::unique_ptr<SshCompression> SshAlgorithmRegistry::create_compression(const std::string & name) const
    {
        auto it = compression_factories_.find(name);
        if (it == compression_factories_.end())
            return nullptr;
        return it->second();
    }

    std::vector<std::string> SshAlgorithmRegistry::supported_kex_names() const
    {
        std::vector<std::string> names;
        names.reserve(kex_factories_.size());
        for (const auto &kv : kex_factories_)
            names.push_back(kv.first);
        return names;
    }

    std::vector<std::string> SshAlgorithmRegistry::supported_host_key_names() const
    {
        std::vector<std::string> names;
        names.reserve(host_key_factories_.size());
        for (const auto &kv : host_key_factories_)
            names.push_back(kv.first);
        return names;
    }

    std::vector<std::string> SshAlgorithmRegistry::supported_cipher_names() const
    {
        std::vector<std::string> names;
        names.reserve(cipher_factories_.size());
        for (const auto &kv : cipher_factories_)
            names.push_back(kv.first);
        return names;
    }

    std::vector<std::string> SshAlgorithmRegistry::supported_mac_names() const
    {
        std::vector<std::string> names;
        names.reserve(mac_factories_.size());
        for (const auto &kv : mac_factories_)
            names.push_back(kv.first);
        return names;
    }

    std::vector<std::string> SshAlgorithmRegistry::supported_compression_names() const
    {
        std::vector<std::string> names;
        names.reserve(compression_factories_.size());
        for (const auto &kv : compression_factories_)
            names.push_back(kv.first);
        return names;
    }

    std::optional<std::string> SshAlgorithmRegistry::negotiate_name_list(
        const std::vector<std::string> & our_prefs,
        const std::string & peer_list)
    {
        std::vector<std::string> peer_algos;
        std::istringstream ss(peer_list);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty())
                peer_algos.push_back(token);
        }

        for (const auto &peer : peer_algos) {
            for (const auto &pref : our_prefs) {
                if (pref == peer)
                    return peer;
            }
        }

        return std::nullopt;
    }

    std::optional<SshNegotiatedAlgorithms> SshAlgorithmRegistry::negotiate(
        const std::vector<std::string> & our_kex_prefs,
        const std::vector<std::string> & our_host_key_prefs,
        const std::vector<std::string> & our_cipher_prefs,
        const std::vector<std::string> & our_mac_prefs,
        const std::vector<std::string> & our_compression_prefs,
        const std::string & peer_kex_algorithms,
        const std::string & peer_host_key_algorithms,
        const std::string & peer_encryption_c2s,
        const std::string & peer_encryption_s2c,
        const std::string & peer_mac_c2s,
        const std::string & peer_mac_s2c,
        const std::string & peer_compression_c2s,
        const std::string & peer_compression_s2c) const
    {
        SshNegotiatedAlgorithms result;

        auto kex = negotiate_name_list(our_kex_prefs, peer_kex_algorithms);
        if (!kex)
            return std::nullopt;
        result.kex_name = std::move(*kex);

        if (result.kex_name.find("sha512") != std::string::npos ||
            result.kex_name.find("sha2-512") != std::string::npos) {
            result.kex_hash_name = "sha512";
        } else if (result.kex_name.find("sha384") != std::string::npos) {
            result.kex_hash_name = "sha384";
        } else if (result.kex_name.find("sha1") != std::string::npos) {
            result.kex_hash_name = "sha1";
        } else {
            result.kex_hash_name = "sha256";
        }

        auto host_key = negotiate_name_list(our_host_key_prefs, peer_host_key_algorithms);
        if (!host_key)
            return std::nullopt;
        result.host_key_name = std::move(*host_key);

        auto cipher_c2s = negotiate_name_list(our_cipher_prefs, peer_encryption_c2s);
        if (!cipher_c2s)
            return std::nullopt;
        result.client_to_server_cipher_name = std::move(*cipher_c2s);

        auto cipher_s2c = negotiate_name_list(our_cipher_prefs, peer_encryption_s2c);
        if (!cipher_s2c)
            return std::nullopt;
        result.server_to_client_cipher_name = std::move(*cipher_s2c);

        auto mac_c2s = negotiate_name_list(our_mac_prefs, peer_mac_c2s);
        if (!mac_c2s)
            return std::nullopt;
        result.client_to_server_mac_name = std::move(*mac_c2s);

        auto mac_s2c = negotiate_name_list(our_mac_prefs, peer_mac_s2c);
        if (!mac_s2c)
            return std::nullopt;
        result.server_to_client_mac_name = std::move(*mac_s2c);

        auto comp_c2s = negotiate_name_list(our_compression_prefs, peer_compression_c2s);
        if (!comp_c2s)
            return std::nullopt;
        result.client_to_server_compression_name = std::move(*comp_c2s);

        auto comp_s2c = negotiate_name_list(our_compression_prefs, peer_compression_s2c);
        if (!comp_s2c)
            return std::nullopt;
        result.server_to_client_compression_name = std::move(*comp_s2c);

        return result;
    }

    void SshAlgorithmRegistry::register_defaults()
    {
        register_compression("none", []()->std::unique_ptr<SshCompression> {
            class NoneCompression : public SshCompression
            {
            public:
                std::string name() const override { return "none"; }
                bool init() override { return true; }
                std::vector<uint8_t> compress(const uint8_t *data, size_t len) override
                {
                    return std::vector<uint8_t>(data, data + len);
                }
                std::vector<uint8_t> decompress(const uint8_t *data, size_t len) override
                {
                    return std::vector<uint8_t>(data, data + len);
                }
            };
            return std::make_unique<NoneCompression>();
        });

        register_kex("curve25519-sha256", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_curve25519();
        });
        register_kex("curve25519-sha256@libssh.org", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_curve25519_libssh();
        });
        register_kex("ecdh-sha2-nistp256", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_ecdh_nistp256();
        });
        register_kex("ecdh-sha2-nistp384", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_ecdh_nistp384();
        });
        register_kex("ecdh-sha2-nistp521", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_ecdh_nistp521();
        });
        register_kex("diffie-hellman-group14-sha256", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_dh_group14();
        });
        register_kex("diffie-hellman-group16-sha512", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_dh_group16();
        });
        register_kex("diffie-hellman-group18-sha512", []()->std::unique_ptr<SshKexAlgorithm> {
            return create_kex_dh_group18();
        });

        register_host_key("ssh-ed25519", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_ed25519();
        });
        register_host_key("ecdsa-sha2-nistp256", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_ecdsa_nistp256();
        });
        register_host_key("ecdsa-sha2-nistp384", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_ecdsa_nistp384();
        });
        register_host_key("ecdsa-sha2-nistp521", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_ecdsa_nistp521();
        });
        register_host_key("rsa-sha2-512", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_rsa_sha512();
        });
        register_host_key("rsa-sha2-256", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_rsa_sha256();
        });
        register_host_key("ssh-rsa", []()->std::unique_ptr<SshHostKeyAlgorithm> {
            return create_host_key_rsa();
        });

        register_cipher("chacha20-poly1305@openssh.com", []()->std::unique_ptr<SshCipher> {
            return create_cipher_chacha20_poly1305();
        });
        register_cipher("aes128-gcm@openssh.com", []()->std::unique_ptr<SshCipher> {
            return create_cipher_aes128_gcm();
        });
        register_cipher("aes256-gcm@openssh.com", []()->std::unique_ptr<SshCipher> {
            return create_cipher_aes256_gcm();
        });
        register_cipher("aes128-ctr", []()->std::unique_ptr<SshCipher> {
            return create_cipher_aes128_ctr();
        });
        register_cipher("aes192-ctr", []()->std::unique_ptr<SshCipher> {
            return create_cipher_aes192_ctr();
        });
        register_cipher("aes256-ctr", []()->std::unique_ptr<SshCipher> {
            return create_cipher_aes256_ctr();
        });

        register_mac("hmac-sha2-256", []()->std::unique_ptr<SshMac> {
            return create_mac_hmac_sha2_256();
        });
        register_mac("hmac-sha2-512", []()->std::unique_ptr<SshMac> {
            return create_mac_hmac_sha2_512();
        });
        register_mac("hmac-sha1", []()->std::unique_ptr<SshMac> {
            return create_mac_hmac_sha1();
        });

#if YUAN_SSH_HAS_ZLIB
        register_compression("zlib", []()->std::unique_ptr<SshCompression> {
            return create_compression_zlib();
        });
        register_compression("zlib@openssh.com", []()->std::unique_ptr<SshCompression> {
            return create_compression_zlib_openssh();
        });
#endif
    }
}
