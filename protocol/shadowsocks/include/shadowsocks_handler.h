#ifndef __NET_SHADOWSOCKS_SHADOWSOCKS_HANDLER_H__
#define __NET_SHADOWSOCKS_SHADOWSOCKS_HANDLER_H__

#include "shadowsocks_protocol.h"

#include <string>

namespace yuan::net::shadowsocks
{
    class ShadowsocksHandler
    {
    public:
        virtual ~ShadowsocksHandler() = default;

        virtual bool on_connect_request(const std::string &client_addr, const TargetAddress &target)
        {
            (void)client_addr;
            (void)target;
            return true;
        }

        virtual void on_session_opened(const std::string &client_addr, const TargetAddress &target)
        {
            (void)client_addr;
            (void)target;
        }

        virtual void on_session_closed(const std::string &client_addr,
                                       const TargetAddress &target,
                                       const std::string &reason)
        {
            (void)client_addr;
            (void)target;
            (void)reason;
        }
    };
}

#endif
