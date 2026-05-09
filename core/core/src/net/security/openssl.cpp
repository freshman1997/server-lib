#include "net/security/openssl.h"
#include "net/security/ssl_handler.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include "openssl/ssl.h"
#include "openssl/err.h"

namespace yuan::net
{
    namespace
    {
        static int alpn_select_callback(SSL *, const unsigned char **out, unsigned char *outlen,
                                         const unsigned char *in, unsigned int inlen, void *arg)
        {
            auto *protocols = static_cast<std::vector<std::string> *>(arg);
            for (const auto &proto : *protocols) {
                const unsigned char *p = in;
                const unsigned char *end = in + inlen;
                while (p < end) {
                    const unsigned char proto_len = *p;
                    if (p + 1 + proto_len > end) {
                        break;
                    }
                    if (proto_len == proto.size() &&
                        std::memcmp(p + 1, proto.data(), proto_len) == 0) {
                        *out = p + 1;
                        *outlen = proto_len;
                        return SSL_TLSEXT_ERR_OK;
                    }
                    p += 1 + proto_len;
                }
            }
            return SSL_TLSEXT_ERR_NOACK;
        }
    }
    static int set_err_msg(const char * msg, size_t len, void * udata)
    {
        OpenSSLModule *this_ = (OpenSSLModule *)udata;
        this_->set_error_msg(msg, len);
        return 0;
    }

    class OpenSSLModule::ModuleData
    {
    public:
        ~ModuleData()
        {
            if (ctx_) {
                SSL_CTX_free(ctx_);
                ctx_ = nullptr;
            }
            delete[] alpn_protocols_storage_;
            alpn_protocols_storage_ = nullptr;
        }

    public:
        std::string errmsg_;
        SSL_CTX *ctx_ = nullptr;
        std::vector<std::string> alpn_protocols_;
        unsigned char *alpn_protocols_storage_ = nullptr;
        std::size_t alpn_protocols_storage_len_ = 0;
    };

    OpenSSLModule::OpenSSLModule()
        : data_(std::make_unique<OpenSSLModule::ModuleData>())
    {
    }

    OpenSSLModule::~OpenSSLModule()
    {
    }

    bool OpenSSLModule::init(const std::string & cert, const std::string & privateKey, SSLHandler::SSLMode mode)
    {
        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
        OpenSSL_add_all_algorithms();

        data_->errmsg_.clear();
        if (data_->ctx_) {
            SSL_CTX_free(data_->ctx_);
            data_->ctx_ = nullptr;
        }

        if (mode == SSLHandler::SSLMode::acceptor_) {
            data_->ctx_ = SSL_CTX_new(TLS_server_method());
        } else {
            data_->ctx_ = SSL_CTX_new(TLS_client_method());
        }

        if (!data_->ctx_) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        if (mode == SSLHandler::SSLMode::acceptor_) {
            if (SSL_CTX_use_certificate_file(data_->ctx_, cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }

            if (SSL_CTX_use_PrivateKey_file(data_->ctx_, privateKey.c_str(), SSL_FILETYPE_PEM) <= 0) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }

            if (!SSL_CTX_check_private_key(data_->ctx_)) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }
        } else {
            if (SSL_CTX_load_verify_locations(data_->ctx_, cert.c_str(), NULL) != 1) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }
        }

        return true;
    }

    void OpenSSLModule::set_alpn_protocols(const std::vector<std::string> &protocols)
    {
        if (!data_->ctx_ || protocols.empty()) {
            return;
        }

        data_->alpn_protocols_ = protocols;

        if (data_->alpn_protocols_storage_) {
            delete[] data_->alpn_protocols_storage_;
        }
        data_->alpn_protocols_storage_ = nullptr;

        std::size_t total = 0;
        for (const auto &p : protocols) {
            total += 1 + p.size();
        }

        auto *wire = new unsigned char[total];
        std::size_t off = 0;
        for (const auto &p : protocols) {
            wire[off++] = static_cast<unsigned char>(p.size());
            std::memcpy(wire + off, p.data(), p.size());
            off += p.size();
        }
        data_->alpn_protocols_storage_ = wire;
        data_->alpn_protocols_storage_len_ = total;

        SSL_CTX_set_alpn_select_cb(data_->ctx_, alpn_select_callback, &data_->alpn_protocols_);
    }

    const std::string *OpenSSLModule::get_error_message() const
    {
        return data_->errmsg_.empty() ? nullptr : &data_->errmsg_;
    }

    std::shared_ptr<SSLHandler> OpenSSLModule::create_handler(int fd, SSLHandler::SSLMode mode)
    {
        if (!data_->ctx_) {
            return nullptr;
        }

        // 创建SSL对象
        SSL *ssl = SSL_new(data_->ctx_);
        if (!ssl) {
            ERR_print_errors_cb(set_err_msg, this);
            return nullptr;
        }

        if (!SSL_set_fd(ssl, fd)) {
            ERR_print_errors_cb(set_err_msg, this);
            SSL_free(ssl);
            return nullptr;
        }

        auto handler = std::make_shared<OpenSSLHandler>();
        handler->set_ssl_data(this, ssl, mode);

        return handler;
    }

    void OpenSSLModule::set_error_msg(const char * msg, size_t len)
    {
        data_->errmsg_.assign(msg, len);
    }

    class OpenSSLHandler::HandlerData
    {
        friend OpenSSLModule;

    public:
        ~HandlerData()
        {
            if (ssl_) {
                SSL_shutdown(ssl_);
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
            module_ = nullptr;
        }

    public:
        OpenSSLHandler::SSLMode mode_;
        SSL *ssl_ = nullptr;
        OpenSSLModule *module_ = nullptr;
    };

    OpenSSLHandler::OpenSSLHandler()
        : data_(std::make_unique<OpenSSLHandler::HandlerData>())
    {
    }

    OpenSSLHandler::~OpenSSLHandler()
    {
    }

    int OpenSSLHandler::ssl_init_action()
    {
        int res = -1;
        if (!data_->ssl_) {
            return res;
        }

        if (data_->mode_ == OpenSSLHandler::SSLMode::acceptor_) {
            res = SSL_accept(data_->ssl_);
        } else {
            res = SSL_connect(data_->ssl_);
        }

        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_, res);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
                return -1;
            }
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        }

        return res;
    }

    bool OpenSSLHandler::ssl_want_read() const
    {
        return data_->ssl_ && SSL_want_read(data_->ssl_);
    }

    bool OpenSSLHandler::ssl_want_write() const
    {
        return data_->ssl_ && SSL_want_write(data_->ssl_);
    }

    std::string_view OpenSSLHandler::get_alpn_selected() const
    {
        if (!data_->ssl_) {
            return {};
        }
        const unsigned char *proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(data_->ssl_, &proto, &len);
        if (!proto || len == 0) {
            return {};
        }
        return {reinterpret_cast<const char *>(proto), len};
    }

    void OpenSSLHandler::set_ssl_data(OpenSSLModule * module, void * ssl, SSLMode mode)
    {
        data_->module_ = module;
        data_->ssl_ = (SSL *)ssl;
        data_->mode_ = mode;
    }

    int OpenSSLHandler::ssl_write(const char * data, std::size_t size)
    {
        if (!data_->module_ || !data_->ssl_ || !data) {
            return -1;
        }

        if (size == 0) {
            return 0;
        }

        int res = SSL_write(data_->ssl_, data, static_cast<int>(size));
        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_, res);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
                return -1;
            }
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        }

        return res;
    }

    int OpenSSLHandler::ssl_read(char * buffer, std::size_t size)
    {
        if (!data_->module_ || !data_->ssl_ || !buffer) {
            return -1;
        }

        if (size == 0) {
            errno = ENOBUFS;
            return -1;
        }

        int res = SSL_read(data_->ssl_, buffer, static_cast<int>(size));
        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_, res);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
                return -1;
            }
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                return 0;
            }
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        }

        return res;
    }
}
