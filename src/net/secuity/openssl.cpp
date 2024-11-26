#include "net/secuity/openssl.h"
#include "net/secuity/ssl_handler.h"

#include <cstring>
#include <memory>
#include "openssl/ssl.h"
#include "openssl/err.h"

namespace net 
{
    static int set_err_msg(const char *msg, size_t len, void *udata)
    {
        OpenSSLModule *this_ = (OpenSSLModule *)udata;
        this_->set_error_msg(msg, len);
        return 0;
    }

    int my_verify_callback(int preverify_ok, X509_STORE_CTX *ctx) 
    {
        // 获取服务器的证书
        X509 *server_cert = X509_STORE_CTX_get_current_cert(ctx);
        if (!server_cert) {
            // 错误处理
            return 0;
        }

        // 在这里实现自定义的证书验证逻辑
        // 例如，可以检查证书的颁发机构、有效期等
        // 如果验证通过，返回 1；否则返回 0
        // 注意：这里只是一个示例，实际应用中需要更严格的验证逻辑
        return preverify_ok;
    }

    class OpenSSLModule::ModuleData
    {
    public:
        ~ModuleData()
        {
            if (ctx_) {
                SSL_CTX_free(ctx_);
                ERR_free_strings();
                EVP_cleanup();
                CRYPTO_cleanup_all_ex_data();
                ctx_ = nullptr;
            }

            if (errmsg_) {
                delete errmsg_;
                errmsg_ = nullptr;
            }
        }

    public:
        bool believe_self_sign_ = false;
        std::string *errmsg_ = nullptr;
        SSL_CTX *ctx_ = nullptr;
    };

    OpenSSLModule::OpenSSLModule() : data_(std::make_unique<OpenSSLModule::ModuleData>()) {}

    OpenSSLModule::~OpenSSLModule()
    {
        
    }

    bool OpenSSLModule::init(const std::string &cert, const std::string &privateKey, SSLHandler::SSLMode mode)
    {
        SSL_library_init();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
        OpenSSL_add_all_algorithms();

        SSL_CTX *ctx = nullptr;
        if (mode == SSLHandler::SSLMode::acceptor_) {
            ctx = SSL_CTX_new(TLS_server_method());
        } else {
            ctx = SSL_CTX_new(TLS_client_method());
        }

        if (!ctx) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        if (SSL_CTX_use_certificate_file(ctx, cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        if (SSL_CTX_use_PrivateKey_file(ctx, privateKey.c_str(), SSL_FILETYPE_PEM) <= 0) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        if (!SSL_CTX_check_private_key(ctx)) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        data_->ctx_ = ctx;

        set_believe_self_sign_ca(data_->believe_self_sign_);
        
        return true;
    }

    const std::string * OpenSSLModule::get_error_message()
    {
        return data_->errmsg_;
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
       
        SSL_set_fd(ssl, fd);
        std::shared_ptr<SSLHandler> handler = std::make_shared<OpenSSLHandler>();
        handler->set_user_data(this, ssl, mode);

        return handler;
    }

    void OpenSSLModule::set_believe_self_sign_ca(bool on)
    {
        data_->believe_self_sign_ = on;
        if (data_->ctx_) {
            if (on) {
                SSL_CTX_set_verify(data_->ctx_, SSL_VERIFY_PEER, my_verify_callback);
            }
        }
    }

    void OpenSSLModule::set_error_msg(const char *msg, size_t len)
    {
        if (data_->errmsg_) {
            delete data_->errmsg_;
        }
        data_->errmsg_ = new std::string(msg, len);
    }

    class OpenSSLHandler::HandlerData
    {
    public:
        ~HandlerData() 
        {
            if (ssl_) {
                SSL_shutdown(ssl_);
                SSL_free(ssl_);
                ERR_free_strings();
                EVP_cleanup();
                ssl_ = nullptr;
            }
            module_ = nullptr;
        }

    public:
        OpenSSLHandler::SSLMode mode_;
        SSL *ssl_ = nullptr;
        OpenSSLModule *module_ = nullptr;
    };

    OpenSSLHandler::OpenSSLHandler() : data_(std::make_unique<OpenSSLHandler::HandlerData>()) {}

    OpenSSLHandler::~OpenSSLHandler() {}

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
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        }

        return res;
    }

    void OpenSSLHandler::set_user_data(void *udata1, void *udata2, SSLMode mode)
    {
        data_->module_ = (OpenSSLModule *)udata1;
        data_->ssl_ = (SSL *)udata2;
        data_->mode_ = mode;
    }

    int OpenSSLHandler::ssl_write(Buffer *buff)
    {
        if (!data_->module_) {
            return -1;
        }

        int res = SSL_write(data_->ssl_, buff->peek(), buff->readable_bytes());
        if (res < 0) {
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        } else if (res != 0) {
            buff->add_read_index(res);
        }

        return res;
    }

    int OpenSSLHandler::ssl_read(Buffer *buff)
    {
        if (!data_->module_) {
            return -1;
        }

        int res = SSL_read(data_->ssl_, buff->buffer_begin(), buff->writable_size());
        if (res < 0) {
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        } else if (res != 0) {
            buff->fill(res);
        }

        return res;
    }
}