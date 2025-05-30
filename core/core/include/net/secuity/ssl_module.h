#ifndef __NET_SECUITY_SSL_ACTION_H__
#define __NET_SECUITY_SSL_ACTION_H__
#include <memory>
#include <string>
#include "ssl_handler.h"

namespace yuan::net 
{
    class SSLModule
    {
    public:
        virtual ~SSLModule() {}

    public:
        virtual bool init(const std::string &cert, const std::string &privateKey = {}, SSLHandler::SSLMode mode = SSLHandler::SSLMode::connector_) = 0;

        virtual const std::string * get_error_message() = 0;

        virtual std::shared_ptr<SSLHandler> create_handler(int fd, SSLHandler::SSLMode mode) = 0;
    };
}

#endif
