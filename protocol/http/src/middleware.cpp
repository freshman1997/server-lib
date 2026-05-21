#include "middleware.h"
#include "request.h"
#include "response.h"
#include "response_code.h"
#include "header_key.h"
#include "authorization.h"
#include "base/time.h"

#include <cctype>
#include <mutex>
#include <unordered_map>

namespace yuan::net::http
{
    namespace
    {
        bool token_equals_ci(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i]))) {
                    return false;
                }
            }
            return true;
        }

        std::string_view trim_token(std::string_view value)
        {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
                value.remove_suffix(1);
            }
            return value;
        }

        bool header_has_token(std::string_view value, std::string_view token)
        {
            std::size_t pos = 0;
            while (pos <= value.size()) {
                const auto comma = value.find(',', pos);
                const auto end = comma == std::string_view::npos ? value.size() : comma;
                if (token_equals_ci(trim_token(value.substr(pos, end - pos)), token)) {
                    return true;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                pos = comma + 1;
            }
            return false;
        }
    }

    // ==================== MiddlewarePipeline ====================

    void MiddlewarePipeline::add(std::shared_ptr<HttpMiddleware> middleware)
    {
        if (middleware) {
            middlewares_.push_back(std::move(middleware));
        }
    }

    void MiddlewarePipeline::add(middleware_function fn, const char * name)
    {
        if (fn) {
            middlewares_.push_back(std::make_shared<FunctionMiddleware>(std::move(fn), name));
        }
    }

    void MiddlewarePipeline::insert_front(std::shared_ptr<HttpMiddleware> middleware)
    {
        if (middleware) {
            middlewares_.insert(middlewares_.begin(), std::move(middleware));
        }
    }

    bool MiddlewarePipeline::execute(HttpRequest * req, HttpResponse * resp) const
    {
        for (auto &mw : middlewares_) {
            auto result = mw->process(req, resp);
            switch (result) {
            case MiddlewareResult::next:
                break;
            case MiddlewareResult::stop:
                return false;
            case MiddlewareResult::unauthorized:
                resp->set_response_code(ResponseCode::unauthorized);
                resp->add_header("WWW-Authenticate", "Basic realm=\"Secure Area\"");
                resp->process_error(ResponseCode::unauthorized);
                return false;
            case MiddlewareResult::forbidden:
                resp->process_error(ResponseCode::forbidden);
                return false;
            }
        }
        return true; // 所有中间件通过，继续执行handler
    }

    // ==================== 内置中间件实现 ====================

    namespace middlewares
    {
        // CORS 中间件
        class CorsMiddleware : public HttpMiddleware
        {
        public:
            explicit CorsMiddleware(const CorsConfig &config)
                : config_(config)
            {
            }

            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                // OPTIONS 预检请求直接返回200
                if (req->is_options()) {
                    resp->set_response_code(ResponseCode::no_content);
                    resp->add_header("Access-Control-Allow-Origin", config_.allow_origin);
                    resp->add_header("Access-Control-Allow-Methods", config_.allow_methods);
                    resp->add_header("Access-Control-Allow-Headers", config_.allow_headers);
                    if (config_.max_age > 0) {
                        resp->add_header("Access-Control-Max-Age", std::to_string(config_.max_age));
                    }
                    if (config_.allow_credentials) {
                        resp->add_header("Access-Control-Allow-Credentials", "true");
                    }
                    return MiddlewareResult::stop;
                }

                // 普通请求添加CORS头
                resp->add_header("Access-Control-Allow-Origin", config_.allow_origin);
                resp->add_header("Access-Control-Allow-Methods", config_.allow_methods);
                resp->add_header("Access-Control-Allow-Headers", config_.allow_headers);

                const std::string *origin = req->get_header(http_header_key::origin);
                if (config_.allow_credentials) {
                    if (origin && !origin->empty()) {
                        resp->add_header("Access-Control-Allow-Origin", *origin);
                        resp->add_header("Vary", "Origin");
                    }
                    resp->add_header("Access-Control-Allow-Credentials", "true");
                } else if (origin) {
                    if (config_.allow_origin != "*" && *origin != config_.allow_origin) {
                        return MiddlewareResult::forbidden;
                    }
                }

                return MiddlewareResult::next;
            }

            const char *name() const override
            {
                return "CORS";
            }

        private:
            CorsConfig config_;
        };

        std::shared_ptr<HttpMiddleware> cors(const CorsConfig &config)
        {
            return std::make_shared<CorsMiddleware>(config);
        }

        // Basic Auth 中间件
        class BasicAuthMiddleware : public HttpMiddleware
        {
        public:
            using VerifierFn = std::function<bool(const std::string &, const std::string &)>;

            BasicAuthMiddleware(const VerifierFn &verifier, const std::string &realm)
                : verifier_(verifier), realm_(realm)
            {
            }

            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                const std::string *auth = req->get_header(http_header_key::authorization);
                if (!auth || auth->empty()) {
                    resp->add_header("WWW-Authenticate", "Basic realm=\"" + realm_ + "\"");
                    return MiddlewareResult::unauthorized;
                }

                auto[
                    type,
                    credentials
                ] = HttpAuthorization::decode_authorization_value(*auth);
                if (type == authorization_type::basic) {
                    if (verifier_ && verifier_(credentials.first, credentials.second)) {
                        return MiddlewareResult::next;
                    }
                }

                resp->add_header("WWW-Authenticate", "Basic realm=\"" + realm_ + "\"");
                return MiddlewareResult::unauthorized;
            }

            const char *name() const override
            {
                return "BasicAuth";
            }

        private:
            VerifierFn verifier_;
            std::string realm_;
        };

        std::shared_ptr<HttpMiddleware> basic_auth(
            const std::function<bool(const std::string &, const std::string &)> &verifier,
            const std::string &realm)
        {
            return std::make_shared<BasicAuthMiddleware>(verifier, realm);
        }

        // Bearer Token 中间件
        class BearerAuthMiddleware : public HttpMiddleware
        {
        public:
            explicit BearerAuthMiddleware(const std::function<bool(const std::string &)> &verifier)
                : verifier_(verifier)
            {
            }

            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                const std::string *auth = req->get_header(http_header_key::authorization);
                if (!auth || auth->empty()) {
                    resp->add_header("WWW-Authenticate", "Bearer");
                    return MiddlewareResult::unauthorized;
                }

                // 解析 Bearer token: "Bearer <token>"
                static constexpr std::string_view prefix = "Bearer ";
                if (auth->size() <= prefix.size() ||
                    std::string_view(*auth).substr(0, prefix.size()) != prefix) {
                    resp->add_header("WWW-Authenticate", "Bearer");
                    return MiddlewareResult::unauthorized;
                }

                std::string token = auth->substr(prefix.size());
                if (verifier_ && verifier_(token)) {
                    return MiddlewareResult::next;
                }

                resp->add_header("WWW-Authenticate", "Bearer, error=\"invalid_token\"");
                return MiddlewareResult::unauthorized;
            }

            const char *name() const override
            {
                return "BearerAuth";
            }

        private:
            std::function<bool(const std::string &)> verifier_;
        };

        std::shared_ptr<HttpMiddleware> bearer_auth(
            const std::function<bool(const std::string &)> &verifier)
        {
            return std::make_shared<BearerAuthMiddleware>(verifier);
        }

        // 日志中间件
        class LoggerMiddleware : public HttpMiddleware
        {
        public:
            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                auto start = base::time::steady_now_ms();

                // 记录请求开始 - 通过返回next让后续handler处理后，在response中记录结束
                // 这里我们简单地在进入时记录
                (void)start;

                return MiddlewareResult::next;
            }

            const char *name() const override
            {
                return "Logger";
            }
        };

        std::shared_ptr<HttpMiddleware> logger()
        {
            return std::make_shared<LoggerMiddleware>();
        }

        // 限流中间件（基于IP的滑动窗口）
        class RateLimitMiddleware : public HttpMiddleware
        {
        public:
            RateLimitMiddleware(int max_rps, int burst_size)
                : max_rps_(max_rps), burst_size_(burst_size)
            {
            }

            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                std::string ip = req->get_peer_ip();
                auto now_ms = static_cast<int64_t>(base::time::steady_now_ms());

                std::lock_guard<std::mutex> lock(mutex_);

                // 清理过期的条目
                cleanup_expired(now_ms);

                auto it = requests_.find(ip);
                if (it == requests_.end()) {
                    requests_[ip] = { 1, now_ms, 0 };
                    return MiddlewareResult::next;
                }

                auto &[
                    count,
                    window_start,
                    burst_count
                ] = it->second;

                if (now_ms - window_start > 1000) {
                    count = 1;
                    window_start = now_ms;
                    burst_count = 0;
                    return MiddlewareResult::next;
                }

                if (static_cast<int>(count) >= max_rps_) {
                    if (burst_count < burst_size_) {
                        ++burst_count;
                        ++count;
                        return MiddlewareResult::next;
                    }
                    resp->set_response_code(ResponseCode::too_many_requests);
                    resp->add_header("Retry-After", "1");
                    resp->process_error(ResponseCode::too_many_requests);
                    return MiddlewareResult::stop;
                }

                ++count;
                return MiddlewareResult::next;
            }

            const char *name() const override
            {
                return "RateLimit";
            }

        private:
            void cleanup_expired(int64_t now_ms)
            {
                for (auto it = requests_.begin(); it != requests_.end();) {
                    if (now_ms - std::get<1>(it->second) > 10000) {
                        it = requests_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            int max_rps_;
            int burst_size_;
            std::mutex mutex_;
            std::unordered_map<std::string, std::tuple<size_t, int64_t, int> > requests_;
        };

        std::shared_ptr<HttpMiddleware> rate_limit(int max_rps, int burst_size)
        {
            return std::make_shared<RateLimitMiddleware>(max_rps, burst_size);
        }

        // Body大小限制
        class BodyLimitMiddleware : public HttpMiddleware
        {
        public:
            explicit BodyLimitMiddleware(size_t max_size)
                : max_size_(max_size)
            {
            }

            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                const std::string *cl = req->get_header(http_header_key::content_length);
                if (cl) {
                    try
                    {
                        size_t len = std::stoull(*cl);
                        if (len > max_size_) {
                            resp->set_response_code(ResponseCode::payload_too_large);
                            resp->process_error(ResponseCode::payload_too_large);
                            return MiddlewareResult::stop;
                        }
                    }
                    catch (...)
                    {
                    }
                }
                return MiddlewareResult::next;
            }

            const char *name() const override
            {
                return "BodyLimit";
            }

        private:
            size_t max_size_;
        };

        std::shared_ptr<HttpMiddleware> body_limit(size_t max_size)
        {
            return std::make_shared<BodyLimitMiddleware>(max_size);
        }

        // Connection 处理中间件（Keep-Alive等）
        class ConnectionHandlerMiddleware : public HttpMiddleware
        {
        public:
            MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
            {
                // HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close
                const std::string *conn = req->get_header(http_header_key::connection);
                if (conn) {
                    if (header_has_token(*conn, "close")) {
                        resp->add_header("Connection", "close");
                    } else if (header_has_token(*conn, "keep-alive")) {
                        resp->add_header("Connection", "keep-alive");
                        resp->add_header("Keep-Alive", "timeout=60, max=1000");
                    }
                } else if (req->get_version() == HttpVersion::v_1_0) {
                    // HTTP/1.0 默认关闭连接
                    resp->add_header("Connection", "close");
                } else {
                    // HTTP/1.1+ 默认保持连接
                    resp->add_header("Connection", "keep-alive");
                    resp->add_header("Keep-Alive", "timeout=60, max=1000");
                }

                return MiddlewareResult::next;
            }

            const char *name() const override
            {
                return "ConnectionHandler";
            }
        };

        std::shared_ptr<HttpMiddleware> connection_handler()
        {
            return std::make_shared<ConnectionHandlerMiddleware>();
        }
    }
}
