#include "net/security/openssl.h"
#include "net/security/ssl_handler.h"

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <vector>
#include "openssl/ssl.h"
#include "openssl/err.h"
#include "openssl/x509_vfy.h"
#include "openssl/x509v3.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace yuan::net
{
    namespace
    {
        std::string normalize_tls_version(std::string value)
        {
            value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
                            return std::isspace(ch) != 0 || ch == '_' || ch == '-';
                        }),
                        value.end());
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        }

        int tls_version_constant(const std::string &version)
        {
            const auto v = normalize_tls_version(version);
            if (v.empty() || v == "default" || v == "auto") {
                return 0;
            }
            if (v == "tlsv1" || v == "tls1" || v == "1.0" || v == "1") {
                return TLS1_VERSION;
            }
            if (v == "tlsv1.1" || v == "tls1.1" || v == "1.1") {
                return TLS1_1_VERSION;
            }
            if (v == "tlsv1.2" || v == "tls1.2" || v == "1.2") {
                return TLS1_2_VERSION;
            }
#ifdef TLS1_3_VERSION
            if (v == "tlsv1.3" || v == "tls1.3" || v == "1.3") {
                return TLS1_3_VERSION;
            }
#endif
            return -1;
        }

        struct SslCtxDeleter
        {
            void operator()(SSL_CTX *ctx) const noexcept
            {
                if (ctx) {
                    SSL_CTX_free(ctx);
                }
            }
        };

        struct SslDeleter
        {
            void operator()(SSL *ssl) const noexcept
            {
                if (ssl) {
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                }
            }
        };

        using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
        using SslPtr = std::unique_ptr<SSL, SslDeleter>;

#ifdef _WIN32
        bool load_windows_system_roots(SSL_CTX *ctx)
        {
            auto *store = SSL_CTX_get_cert_store(ctx);
            if (!store) {
                return false;
            }

            HCERTSTORE system_store = CertOpenSystemStoreA(0, "ROOT");
            if (!system_store) {
                return false;
            }

            bool loaded_any = false;
            PCCERT_CONTEXT cert_context = nullptr;
            while ((cert_context = CertEnumCertificatesInStore(system_store, cert_context)) != nullptr) {
                const unsigned char *encoded = cert_context->pbCertEncoded;
                X509 *cert = d2i_X509(nullptr, &encoded, static_cast<long>(cert_context->cbCertEncoded));
                if (!cert) {
                    continue;
                }
                if (X509_STORE_add_cert(store, cert) == 1) {
                    loaded_any = true;
                } else {
                    const auto err = ERR_peek_last_error();
                    if (ERR_GET_LIB(err) == ERR_LIB_X509 && ERR_GET_REASON(err) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
                        ERR_clear_error();
                        loaded_any = true;
                    }
                }
                X509_free(cert);
            }

            CertCloseStore(system_store, 0);
            return loaded_any;
        }
#endif

        bool load_client_trust_roots(SSL_CTX *ctx, const std::string &cert)
        {
            if (!cert.empty()) {
                return SSL_CTX_load_verify_locations(ctx, cert.c_str(), nullptr) == 1;
            }

            bool loaded = SSL_CTX_set_default_verify_paths(ctx) == 1;
#ifdef _WIN32
            loaded = load_windows_system_roots(ctx) || loaded;
#endif
            return loaded;
        }

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
        std::string errmsg_;
        SslCtxPtr ctx_;
        std::vector<std::string> alpn_protocols_;
        std::vector<unsigned char> alpn_protocols_storage_;
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
        data_->ctx_.reset();

        if (mode == SSLHandler::SSLMode::acceptor_) {
            data_->ctx_.reset(SSL_CTX_new(TLS_server_method()));
        } else {
            data_->ctx_.reset(SSL_CTX_new(TLS_client_method()));
        }

        if (!data_->ctx_) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }

        if (mode == SSLHandler::SSLMode::acceptor_) {
            if (SSL_CTX_use_certificate_file(data_->ctx_.get(), cert.c_str(), SSL_FILETYPE_PEM) <= 0) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }

            if (SSL_CTX_use_PrivateKey_file(data_->ctx_.get(), privateKey.c_str(), SSL_FILETYPE_PEM) <= 0) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }

            if (!SSL_CTX_check_private_key(data_->ctx_.get())) {
                ERR_print_errors_cb(set_err_msg, this);
                return false;
            }
        } else {
            SSL_CTX_set_verify(data_->ctx_.get(), SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_dir(data_->ctx_.get());
            if (!load_client_trust_roots(data_->ctx_.get(), cert)) {
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

        std::size_t total = 0;
        for (const auto &p : protocols) {
            total += 1 + p.size();
        }

        data_->alpn_protocols_storage_.assign(total, 0);
        std::size_t off = 0;
        for (const auto &p : protocols) {
            data_->alpn_protocols_storage_[off++] = static_cast<unsigned char>(p.size());
            std::memcpy(data_->alpn_protocols_storage_.data() + off, p.data(), p.size());
            off += p.size();
        }

        SSL_CTX_set_alpn_select_cb(data_->ctx_.get(), alpn_select_callback, &data_->alpn_protocols_);
    }

    bool OpenSSLModule::set_min_protocol_version(const std::string &version)
    {
        if (!data_->ctx_ || version.empty()) {
            return true;
        }
        const int parsed = tls_version_constant(version);
        if (parsed < 0) {
            data_->errmsg_ = "unsupported TLS min protocol version: " + version;
            return false;
        }
        if (SSL_CTX_set_min_proto_version(data_->ctx_.get(), parsed) != 1) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }
        return true;
    }

    bool OpenSSLModule::set_max_protocol_version(const std::string &version)
    {
        if (!data_->ctx_ || version.empty()) {
            return true;
        }
        const int parsed = tls_version_constant(version);
        if (parsed < 0) {
            data_->errmsg_ = "unsupported TLS max protocol version: " + version;
            return false;
        }
        if (SSL_CTX_set_max_proto_version(data_->ctx_.get(), parsed) != 1) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }
        return true;
    }

    bool OpenSSLModule::set_cipher_list(const std::string &ciphers)
    {
        if (!data_->ctx_ || ciphers.empty()) {
            return true;
        }
        if (SSL_CTX_set_cipher_list(data_->ctx_.get(), ciphers.c_str()) != 1) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }
        return true;
    }

    bool OpenSSLModule::set_ciphersuites(const std::string &ciphersuites)
    {
        if (!data_->ctx_ || ciphersuites.empty()) {
            return true;
        }
#ifdef TLS1_3_VERSION
        if (SSL_CTX_set_ciphersuites(data_->ctx_.get(), ciphersuites.c_str()) != 1) {
            ERR_print_errors_cb(set_err_msg, this);
            return false;
        }
#else
        data_->errmsg_ = "TLS 1.3 ciphersuites are not supported by this OpenSSL build";
        return false;
#endif
        return true;
    }

    void OpenSSLModule::set_prefer_server_ciphers(bool enabled)
    {
        if (!data_->ctx_) {
            return;
        }
        if (enabled) {
            SSL_CTX_set_options(data_->ctx_.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        } else {
            SSL_CTX_clear_options(data_->ctx_.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
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
        SslPtr ssl(SSL_new(data_->ctx_.get()));
        if (!ssl) {
            ERR_print_errors_cb(set_err_msg, this);
            return nullptr;
        }

        if (!SSL_set_fd(ssl.get(), fd)) {
            ERR_print_errors_cb(set_err_msg, this);
            return nullptr;
        }

        auto handler = std::make_shared<OpenSSLHandler>();
        handler->set_ssl_data(this, ssl.release(), mode);

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
        OpenSSLHandler::SSLMode mode_;
        SslPtr ssl_;
        OpenSSLModule *module_ = nullptr;
        bool want_read_ = false;
        bool want_write_ = false;

        void clear_last_want() noexcept
        {
            want_read_ = false;
            want_write_ = false;
        }

        bool set_last_error_want(int ssl_error) noexcept
        {
            want_read_ = ssl_error == SSL_ERROR_WANT_READ;
            want_write_ = ssl_error == SSL_ERROR_WANT_WRITE;
            return want_read_ || want_write_;
        }
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
        data_->clear_last_want();

        if (data_->mode_ == OpenSSLHandler::SSLMode::acceptor_) {
            res = SSL_accept(data_->ssl_.get());
        } else {
            res = SSL_connect(data_->ssl_.get());
        }

        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_.get(), res);
            if (data_->set_last_error_want(ssl_error)) {
                errno = EAGAIN;
                return -1;
            }
            ERR_print_errors_cb(set_err_msg, this->data_->module_);
        } else if (data_->mode_ == OpenSSLHandler::SSLMode::connector_) {
            const auto verify_result = SSL_get_verify_result(data_->ssl_.get());
            if (verify_result != X509_V_OK) {
                if (data_->module_) {
                    const auto *message = X509_verify_cert_error_string(verify_result);
                    data_->module_->set_error_msg(message, std::strlen(message));
                }
                return -1;
            }
        }

        return res;
    }

    bool OpenSSLHandler::set_hostname(const std::string &hostname)
    {
        if (!data_->ssl_ || hostname.empty()) {
            return false;
        }
        if (SSL_set_tlsext_host_name(data_->ssl_.get(), hostname.c_str()) != 1) {
            return false;
        }
        auto *param = SSL_get0_param(data_->ssl_.get());
        if (!param) {
            return false;
        }
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        return X509_VERIFY_PARAM_set1_host(param, hostname.c_str(), hostname.size()) == 1;
    }

    bool OpenSSLHandler::ssl_want_read() const
    {
        return data_->ssl_ && data_->want_read_;
    }

    bool OpenSSLHandler::ssl_want_write() const
    {
        return data_->ssl_ && data_->want_write_;
    }

    std::string_view OpenSSLHandler::get_alpn_selected() const
    {
        if (!data_->ssl_) {
            return {};
        }
        const unsigned char *proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(data_->ssl_.get(), &proto, &len);
        if (!proto || len == 0) {
            return {};
        }
        return {reinterpret_cast<const char *>(proto), len};
    }

    void OpenSSLHandler::set_ssl_data(OpenSSLModule * module, SSL *ssl, SSLMode mode)
    {
        data_->module_ = module;
        data_->ssl_.reset(ssl);
        data_->mode_ = mode;
    }

    int OpenSSLHandler::ssl_write(const char * data, std::size_t size)
    {
        if (!data_->module_ || !data_->ssl_ || !data) {
            return -1;
        }
        data_->clear_last_want();

        if (size == 0) {
            return 0;
        }

        int res = SSL_write(data_->ssl_.get(), data, static_cast<int>(size));
        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_.get(), res);
            if (data_->set_last_error_want(ssl_error)) {
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
        data_->clear_last_want();

        if (size == 0) {
            errno = ENOBUFS;
            return -1;
        }

        int res = SSL_read(data_->ssl_.get(), buffer, static_cast<int>(size));
        if (res <= 0) {
            const int ssl_error = SSL_get_error(data_->ssl_.get(), res);
            if (data_->set_last_error_want(ssl_error)) {
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
