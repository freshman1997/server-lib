#ifndef __NET_SECURITY_SSL_ACTION_H__
#define __NET_SECURITY_SSL_ACTION_H__
#include <memory>
#include <string>
#include <vector>
#include "net/security/ssl_handler.h"

namespace yuan::net
{
    class SSLModule
    {
    public:
        virtual ~SSLModule()
        {
        }

    public:
        virtual bool init(const std::string &cert, const std::string &privateKey = {}, SSLHandler::SSLMode mode = SSLHandler::SSLMode::connector_) = 0;

        virtual const std::string *get_error_message() const = 0;

        virtual std::shared_ptr<SSLHandler> create_handler(int fd, SSLHandler::SSLMode mode) = 0;

        virtual void set_alpn_protocols(const std::vector<std::string> &protocols) = 0;

        virtual bool set_min_protocol_version(const std::string &version)
        {
            return version.empty();
        }

        virtual bool set_max_protocol_version(const std::string &version)
        {
            return version.empty();
        }

        virtual bool set_cipher_list(const std::string &ciphers)
        {
            return ciphers.empty();
        }

        virtual bool set_ciphersuites(const std::string &ciphersuites)
        {
            return ciphersuites.empty();
        }

        virtual void set_prefer_server_ciphers(bool enabled)
        {
            (void)enabled;
        }
    };
}

#endif
