#ifndef __ENDIAN_H__
#define __ENDIAN_H__

#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>
#include <cstdlib>
#elif defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#include <netinet/in.h>
#else
#include <arpa/inet.h>
#include <byteswap.h>
#include <endian.h>
#endif

namespace yuan::endian
{
    constexpr bool isLittleEndian() noexcept
    {
#if defined(_WIN32)
        return true;
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
        return __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
        return true;
#endif
    }

    inline uint16_t byteSwap16(const uint16_t value) noexcept
    {
#if defined(_WIN32)
        return _byteswap_ushort(value);
#elif defined(__APPLE__)
        return OSSwapInt16(value);
#else
        return bswap_16(value);
#endif
    }

    inline uint32_t byteSwap32(const uint32_t value) noexcept
    {
#if defined(_WIN32)
        return _byteswap_ulong(value);
#elif defined(__APPLE__)
        return OSSwapInt32(value);
#else
        return bswap_32(value);
#endif
    }

    inline uint64_t byteSwap64(const uint64_t value) noexcept
    {
#if defined(_WIN32)
        return _byteswap_uint64(value);
#elif defined(__APPLE__)
        return OSSwapInt64(value);
#else
        return bswap_64(value);
#endif
    }

    inline uint64_t htonll(const uint64_t value) noexcept
    {
        return isLittleEndian() ? byteSwap64(value) : value;
    }

    inline uint64_t ntohll(const uint64_t value) noexcept
    {
        return htonll(value);
    }

    inline uint64_t hostToNetwork64(const uint64_t host64) noexcept
    {
        return htonll(host64);
    }

    inline uint32_t hostToNetwork32(const uint32_t host32) noexcept
    {
        return htonl(host32);
    }

    inline uint16_t hostToNetwork16(const uint16_t host16) noexcept
    {
        return htons(host16);
    }

    inline uint64_t networkToHost64(const uint64_t net64) noexcept
    {
        return ntohll(net64);
    }

    inline uint32_t networkToHost32(const uint32_t net32) noexcept
    {
        return ntohl(net32);
    }

    inline uint16_t networkToHost16(const uint16_t net16) noexcept
    {
        return ntohs(net16);
    }
}

#endif
