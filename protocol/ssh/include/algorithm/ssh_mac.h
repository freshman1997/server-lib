#ifndef __NET_SSH_ALGORITHM_SSH_MAC_H__
#define __NET_SSH_ALGORITHM_SSH_MAC_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshMac
    {
    public:
        virtual ~SshMac() = default;

        virtual std::string name() const = 0;

        virtual size_t digest_size() const = 0;

        virtual size_t key_size() const = 0;

        virtual void init(const std::vector<uint8_t> &key) = 0;

        virtual std::vector<uint8_t> compute(uint32_t seq,
                                             const uint8_t *data, size_t len) = 0;

        virtual bool verify(uint32_t seq,
                            const uint8_t *data, size_t len,
                            const uint8_t *mac, size_t mac_len) = 0;
    };
}

#endif
