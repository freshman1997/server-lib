#ifndef __ENDIAN_H__
#define __ENDIAN_H__
#include <stdint.h>

#ifndef _WIN32
#include <endian.h>
#endif

// TODO win 下的实现

namespace endian
{
    // the inline assembler code makes type blur,
    // so we disable warnings for a while.
#ifndef _WIN32
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wconversion"
    #pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    inline uint64_t hostToNetwork64(uint64_t host64)
    {
        #ifdef _WIN32
        return htonl(host64);
        #else
        return htobe64(host64);
        #endif
    }

    inline uint32_t hostToNetwork32(uint32_t host32)
    {
        #ifdef _WIN32
        return htonl(host32);
        #else
        return htobe32(host32);
        #endif
    }

    inline uint16_t hostToNetwork16(uint16_t host16)
    {
        #ifdef _WIN32
        return htons(host16);
        #else
        return htobe16(host16);
        #endif
    }

    inline uint64_t networkToHost64(uint64_t net64)
    {
        #ifdef _WIN32
        return ntohl(net64);
        #else
        return be64toh(net64);
        #endif
    }

    inline uint32_t networkToHost32(uint32_t net32)
    {
        #ifdef _WIN32
        return ntohl(net32);
        #else
        return be32toh(net32);
        #endif
    }

    inline uint16_t networkToHost16(uint16_t net16)
    {
        #ifdef _WIN32
        return ntohs(net16);
        #else
        return be16toh(net16);
        #endif
    }

#ifndef _WIN32
    #pragma GCC diagnostic pop
#endif
}

#endif
