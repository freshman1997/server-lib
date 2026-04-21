#ifndef __YUAN_NET_IP_POLICY_H__
#define __YUAN_NET_IP_POLICY_H__

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <cstdint>
#include <string>

namespace yuan::net
{

    inline bool is_private_ip(const std::string &ip)
    {
        if (ip.empty()) {
            return false;
        }

        if (ip.find(':') != std::string::npos) {
            in6_addr addr6{};
            if (inet_pton(AF_INET6, ip.c_str(), &addr6) != 1) {
                return false;
            }

            const auto *b = addr6.s6_addr;

            bool all_zero = true;
            for (int i = 0; i < 16; ++i) {
                if (b[i] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero) {
                return true;
            }

            bool only_last = true;
            for (int i = 0; i < 15; ++i) {
                if (b[i] != 0) {
                    only_last = false;
                    break;
                }
            }
            if (only_last && b[15] == 1) {
                return true;
            }

            if ((b[0] & 0xfe) == 0xfc) {
                return true;
            }

            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) {
                return true;
            }

            return false;
        }

        in_addr addr4{};
        if (inet_pton(AF_INET, ip.c_str(), &addr4) != 1) {
            return false;
        }

        const uint32_t ip_num = ntohl(addr4.s_addr);
        const uint8_t a = (ip_num >> 24) & 0xFF;
        const uint8_t b = (ip_num >> 16) & 0xFF;

        if (a == 0) return true;
        if (a == 127) return true;
        if (a == 10) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        if (a == 192 && b == 168) return true;
        if (a == 169 && b == 254) return true;
        if (a == 100 && (b & 0xc0) == 0x40) return true;

        return false;
    }

}

#endif
