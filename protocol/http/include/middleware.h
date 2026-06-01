#ifndef __HTTP_MIDDLEWARE_H__
#define __HTTP_MIDDLEWARE_H__

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace yuan::net::http
{
    class HttpRequest;
    class HttpResponse;

    // 中间件上下文 - 控制请求流转
    enum class MiddlewareResult
    {
        next,           // 继续执行下一个
        stop,           // 停止执行（已处理响应）
        unauthorized,   // 401 未授权
        forbidden,      // 403 禁止访问
    };

    // 中间件函数类型
    using middleware_function = std::function<MiddlewareResult(HttpRequest *req, HttpResponse *resp)>;

    class HttpMiddleware
    {
    public:
        HttpMiddleware() = default;
        virtual ~HttpMiddleware() = default;

        virtual MiddlewareResult process(HttpRequest *req, HttpResponse *resp) = 0;
        
        // 中间件名称（用于日志/调试）
        virtual const char* name() const = 0;
    };

    // 函数式中间件包装器
    class FunctionMiddleware : public HttpMiddleware
    {
    public:
        explicit FunctionMiddleware(middleware_function fn, const char *name = "anonymous")
            : fn_(std::move(fn)), name_(name) {}

        MiddlewareResult process(HttpRequest *req, HttpResponse *resp) override
        {
            if (fn_) return fn_(req, resp);
            return MiddlewareResult::next;
        }

        const char* name() const override { return name_; }

    private:
        middleware_function fn_;
        const char* name_;
    };

    // 中间件管道
    class MiddlewarePipeline
    {
    public:
        MiddlewarePipeline() = default;
        ~MiddlewarePipeline() = default;

        // 添加中间件（按添加顺序执行）
        void add(std::shared_ptr<HttpMiddleware> middleware);
        void add(middleware_function fn, const char *name = "anonymous");
        
        // 在最前面插入（如全局CORS等）
        void insert_front(std::shared_ptr<HttpMiddleware> middleware);

        // 执行管道，返回是否应该继续处理handler
        bool execute(HttpRequest *req, HttpResponse *resp) const;

        size_t size() const { return middlewares_.size(); }
        bool empty() const { return middlewares_.empty(); }
        void clear() { middlewares_.clear(); }

    private:
        std::vector<std::shared_ptr<HttpMiddleware>> middlewares_;
    };

    // ==================== 内置中间件工厂 ====================

    namespace middlewares 
    {
        // CORS 中间件
        struct CorsConfig
        {
            std::string allow_origin = "*";
            std::string allow_methods = "GET, POST, PUT, DELETE, OPTIONS, PATCH";
            std::string allow_headers = "Content-Type, Authorization, X-Requested-With";
            std::string expose_headers = "Content-Length, Content-Range, Accept-Ranges, ETag, Content-Disposition";
            int max_age = 86400;  // 24小时
            bool allow_credentials = false;
        };
        std::shared_ptr<HttpMiddleware> cors(const CorsConfig &config = {});

        // 认证中间件 (Basic Auth)
        std::shared_ptr<HttpMiddleware> basic_auth(
            const std::function<bool(const std::string &user, const std::string &pass)> &verifier,
            const std::string &realm = "Secure Area");

        // Bearer Token 认证
        std::shared_ptr<HttpMiddleware> bearer_auth(
            const std::function<bool(const std::string &token)> &verifier);

        // 日志中间件
        std::shared_ptr<HttpMiddleware> logger();

        // 限流中间件（基于IP）
        std::shared_ptr<HttpMiddleware> rate_limit(int max_requests_per_second, int burst_size = 10);

        // 请求大小限制中间件
        std::shared_ptr<HttpMiddleware> body_limit(size_t max_size);

        // Keep-Alive / Connection 处理
        std::shared_ptr<HttpMiddleware> connection_handler();
    }
}

#endif
