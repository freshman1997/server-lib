#ifndef __NET_SSH_ALGORITHM_SSH_ALGORITHM_REGISTRY_H__
#define __NET_SSH_ALGORITHM_SSH_ALGORITHM_REGISTRY_H__

#include "algorithm/ssh_kex_algorithm.h"
#include "algorithm/ssh_host_key_algorithm.h"
#include "algorithm/ssh_cipher.h"
#include "algorithm/ssh_mac.h"
#include "algorithm/ssh_compression.h"
#include "protocol/ssh_structures.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::ssh
{
    using KexFactory = std::function<std::unique_ptr<SshKexAlgorithm>()>;
    using HostKeyFactory = std::function<std::unique_ptr<SshHostKeyAlgorithm>()>;
    using CipherFactory = std::function<std::unique_ptr<SshCipher>()>;
    using MacFactory = std::function<std::unique_ptr<SshMac>()>;
    using CompressionFactory = std::function<std::unique_ptr<SshCompression>()>;

    class SshAlgorithmRegistry
    {
    public:
        void register_kex(const std::string &name, KexFactory factory);
        void register_host_key(const std::string &name, HostKeyFactory factory);
        void register_cipher(const std::string &name, CipherFactory factory);
        void register_mac(const std::string &name, MacFactory factory);
        void register_compression(const std::string &name, CompressionFactory factory);

        std::unique_ptr<SshKexAlgorithm> create_kex(const std::string &name) const;
        std::unique_ptr<SshHostKeyAlgorithm> create_host_key(const std::string &name) const;
        std::unique_ptr<SshCipher> create_cipher(const std::string &name) const;
        std::unique_ptr<SshMac> create_mac(const std::string &name) const;
        std::unique_ptr<SshCompression> create_compression(const std::string &name) const;

        std::vector<std::string> supported_kex_names() const;
        std::vector<std::string> supported_host_key_names() const;
        std::vector<std::string> supported_cipher_names() const;
        std::vector<std::string> supported_mac_names() const;
        std::vector<std::string> supported_compression_names() const;

        std::optional<SshNegotiatedAlgorithms> negotiate(
            const std::vector<std::string> &our_kex_prefs,
            const std::vector<std::string> &our_host_key_prefs,
            const std::vector<std::string> &our_cipher_prefs,
            const std::vector<std::string> &our_mac_prefs,
            const std::vector<std::string> &our_compression_prefs,
            const std::string &peer_kex_algorithms,
            const std::string &peer_host_key_algorithms,
            const std::string &peer_encryption_c2s,
            const std::string &peer_encryption_s2c,
            const std::string &peer_mac_c2s,
            const std::string &peer_mac_s2c,
            const std::string &peer_compression_c2s,
            const std::string &peer_compression_s2c) const;

        void register_defaults();

    private:
        static std::optional<std::string> negotiate_name_list(
            const std::vector<std::string> &our_prefs,
            const std::string &peer_list);

        std::unordered_map<std::string, KexFactory> kex_factories_;
        std::unordered_map<std::string, HostKeyFactory> host_key_factories_;
        std::unordered_map<std::string, CipherFactory> cipher_factories_;
        std::unordered_map<std::string, MacFactory> mac_factories_;
        std::unordered_map<std::string, CompressionFactory> compression_factories_;
    };
}

#endif
