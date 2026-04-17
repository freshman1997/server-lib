#ifndef __NET_SSH_HOSTKEY_SSH_HOST_KEY_PROVIDER_H__
#define __NET_SSH_HOSTKEY_SSH_HOST_KEY_PROVIDER_H__

#include "algorithm/ssh_host_key_algorithm.h"
#include "crypto/ssh_crypto.h"
#include "protocol/ssh_constants.h"
#include "protocol/ssh_structures.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshHostKeyProvider
    {
    public:
        bool load_key(const std::string &path, SshHostKeyType type);

        bool load_or_generate(const std::string &path, SshHostKeyType type);

        SshHostKeyAlgorithm *find_algorithm(const std::string &algo_name) const;

        std::vector<std::string> supported_algorithm_names() const;

        SshHostKeyAlgorithm *default_algorithm() const;

        bool generate_key(SshHostKeyType type, const std::string &path);

        static std::string default_key_path(SshHostKeyType type);

    private:
        struct KeyEntry
        {
            SshHostKeyType type;
            std::string algorithm_name;
            std::unique_ptr<SshHostKeyAlgorithm> algorithm;
            std::unique_ptr<SshCrypto> crypto;
        };

        std::vector<KeyEntry> entries_;
    };
}

#endif
