#ifndef __NET_SECUITY_OPENSSL_HANDLER_H__
#define __NET_SECUITY_OPENSSL_HANDLER_H__
#include "net/secuity/ssl_module.h"
#include "net/secuity/ssl_handler.h"
#include <memory>

namespace yuan::net
{
    class OpenSSLModule : public SSLModule
    {
    public:
        OpenSSLModule();
        ~OpenSSLModule();

    public:
        virtual bool init(const std::string &cert, const std::string &privateKey = {}, SSLHandler::SSLMode mode = SSLHandler::SSLMode::connector_);

        virtual const std::string *get_error_message() const override;

        virtual std::shared_ptr<SSLHandler> create_handler(int fd, SSLHandler::SSLMode mode);

        virtual void set_alpn_protocols(const std::vector<std::string> &protocols) override;

    public:
        void set_error_msg(const char *msg, size_t len);

    private:
        class ModuleData;
        std::unique_ptr<ModuleData> data_;
    };

    class OpenSSLHandler : public SSLHandler
    {
        friend class OpenSSLModule;

    public:
        OpenSSLHandler();
        ~OpenSSLHandler();

    public:
        virtual int ssl_init_action();

        virtual int ssl_write(const char *data, std::size_t size);

        virtual int ssl_read(char *buffer, std::size_t size);

        virtual bool ssl_want_read() const override;

        virtual bool ssl_want_write() const override;

        virtual std::string_view get_alpn_selected() const override;

    private:
        void set_ssl_data(OpenSSLModule *module, void *ssl, SSLMode mode);

    private:
        class HandlerData;
        std::unique_ptr<HandlerData> data_;
    };
}

#endif
