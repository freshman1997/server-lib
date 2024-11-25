#ifndef __NET_SECUITY_OPENSSL_HANDLER_H__
#define __NET_SECUITY_OPENSSL_HANDLER_H__
#include "net/secuity/ssl_module.h"
#include "net/secuity/ssl_handler.h"
#include <memory>

namespace net
{
    class OpenSSLModule : public SSLModule
    {
    public:
        OpenSSLModule();
        ~OpenSSLModule();

    public:
        virtual bool init(const std::string &cert, const std::string &privateKey, SSLHandler::SSLMode mode);

        virtual const std::string * get_error_message();

        virtual std::shared_ptr<SSLHandler> create_handler(int fd, SSLHandler::SSLMode mode);

        virtual void set_believe_self_sign_ca(bool on);

    public:
        void set_error_msg(const char *msg, size_t len);

    private:
        class ModuleData;
        std::unique_ptr<ModuleData> data_;
    };

    class OpenSSLHandler : public SSLHandler
    {
    public:
        OpenSSLHandler();
        ~OpenSSLHandler();

    public:
        virtual void set_user_data(void *udata1, void *udata2, SSLMode mode);

        virtual int ssl_init_action();

        virtual int ssl_write(Buffer *buff);

        virtual int ssl_read(Buffer *buff);

    private:
        class HandlerData;
        std::unique_ptr<HandlerData> data_;
    };
}

#endif
