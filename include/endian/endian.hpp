#ifndef __ENDIAN_H__
#define __ENDIAN_H__
#include <stdint.h>

#ifdef __linux__
#include <endian.h>
#include <netinet/in.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <netinet/in.h>
#endif

namespace endian
{
    const union {
        long one;
        char little;
    } ossl_is_endian = { 1 };

#define IS_LITTLE_ENDIAN (ossl_is_endian.little != 0)
#define IS_BIG_ENDIAN    (ossl_is_endian.little == 0)

    inline uint64_t htonll(uint64_t host_longlong) 
    {
        if (IS_LITTLE_ENDIAN) {
            uint32_t high = htonl((uint32_t)(host_longlong >> 32));
            uint32_t low = htonl((uint32_t)host_longlong);
            return ((uint64_t)low) << 32 | high;
        } else {
            uint32_t high = ((host_longlong >> 32) & 0xFFFFFFFF);
            uint32_t low = (host_longlong & 0xFFFFFFFF);
            return ((uint64_t)htonl(low)) << 32 | htonl(high);
        }
    }

    inline uint64_t ntohll(uint64_t net_longlong) 
    {
        if (IS_LITTLE_ENDIAN) {
            uint32_t high = ntohl((uint32_t)(net_longlong >> 32));
            uint32_t low = ntohl((uint32_t)net_longlong);
            return ((uint64_t)low) << 32 | high;
        } else {
            uint32_t high = htonl((uint32_t)(net_longlong >> 32));
            uint32_t low = htonl((uint32_t)net_longlong);
            return ((uint64_t)high) << 32 | low;
        }
    }

    // the inline assembler code makes type blur,
    // so we disable warnings for a while.
#ifdef __linux__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wconversion"
    #pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    inline uint64_t hostToNetwork64(uint64_t host64)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return htonll(host64);
    #else
        return htobe64(host64);
    #endif
    }

    inline uint32_t hostToNetwork32(uint32_t host32)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return htonl(host32);
    #else
        return htobe32(host32);
    #endif
    }

    inline uint16_t hostToNetwork16(uint16_t host16)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return htons(host16);
    #else
        return htobe16(host16);
    #endif
    }

    inline uint64_t networkToHost64(uint64_t net64)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return ntohll(net64);
    #else
        return be64toh(net64);
    #endif
    }

    inline uint32_t networkToHost32(uint32_t net32)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return ntohl(net32);
    #else
        return be32toh(net32);
    #endif
    }

    inline uint16_t networkToHost16(uint16_t net16)
    {
    #if defined(_WIN32) || defined(__APPLE__)
        return ntohs(net16);
    #else
        return be16toh(net16);
    #endif
    }

#ifdef __linux__
    #pragma GCC diagnostic pop
#endif
}

#endif
