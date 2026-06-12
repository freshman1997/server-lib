#ifndef __NET_SECURITY_OPENSSL_HANDLER_H__
#define __NET_SECURITY_OPENSSL_HANDLER_H__
#include "net/security/ssl_module.h"
#include "net/security/ssl_handler.h"
#include <memory>

typedef struct ssl_st SSL;

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
        bool set_min_protocol_version(const std::string &version) override;
        bool set_max_protocol_version(const std::string &version) override;
        bool set_cipher_list(const std::string &ciphers) override;
        bool set_ciphersuites(const std::string &ciphersuites) override;
        void set_prefer_server_ciphers(bool enabled) override;

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
        void set_ssl_data(OpenSSLModule *module, SSL *ssl, SSLMode mode);

    private:
        class HandlerData;
        std::unique_ptr<HandlerData> data_;
    };
}

#endif
