#ifndef __UTILS_H__
#define __UTILS_H__
#include <cstddef>

namespace yuan::bit_torrent
{
    const unsigned char * gen_hash(const unsigned char *input, std::size_t len);
}

#endif // __UTILS_H__