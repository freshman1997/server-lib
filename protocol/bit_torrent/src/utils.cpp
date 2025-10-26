#include "utils.h"
#include "openssl/sha.h"

namespace yuan::bit_torrent
{
    const unsigned char * gen_hash(const unsigned char *input, size_t len)
    {
        unsigned char *hash = SHA1(input, len, nullptr);
        return hash;
    }
}