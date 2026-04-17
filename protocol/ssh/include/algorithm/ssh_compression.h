#ifndef __NET_SSH_ALGORITHM_SSH_COMPRESSION_H__
#define __NET_SSH_ALGORITHM_SSH_COMPRESSION_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::ssh
{
    class SshCompression
    {
    public:
        virtual ~SshCompression() = default;

        virtual std::string name() const = 0;

        virtual bool init() = 0;

        virtual std::vector<uint8_t> compress(const uint8_t *data, size_t len) = 0;

        virtual std::vector<uint8_t> decompress(const uint8_t *data, size_t len) = 0;
    };
}

#endif
