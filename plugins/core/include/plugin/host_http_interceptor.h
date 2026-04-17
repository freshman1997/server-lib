#ifndef __YUAN_PLUGIN_HOST_HTTP_INTERCEPTOR_H__
#define __YUAN_PLUGIN_HOST_HTTP_INTERCEPTOR_H__

#include <cstdint>
#include <functional>
#include <string>

namespace yuan::plugin
{

/// 插件 HTTP 拦截器注册 ID
using HttpInterceptorId = uint64_t;

/// HTTP 中间件回调类型
/// 参数: (请求路径, 请求方法, 请求体指针, 响应体指针)
/// 返回: true 继续链路, false 拦截请求 (已自行填充响应)
using HttpMiddlewareCallback = std::function<bool(
    const std::string &path,
    const std::string &method,
    void *request,   ///< HttpRequest* (类型擦除, 避免核心层依赖协议层)
    void *response   ///< HttpResponse*
)>;

/// HTTP 路由处理器回调类型
using HttpRouteCallback = std::function<void(
    const std::string &path,
    const std::string &method,
    void *request,
    void *response
)>;

/// 插件 HTTP 拦截接口 — 插件通过此接口向宿主 HTTP 服务注册中间件和路由
class HostHttpInterceptor
{
public:
    virtual ~HostHttpInterceptor() = default;

    /// 注册全局中间件 (对所有 HTTP 请求生效)
    /// 返回拦截器 ID, 0 表示失败
    virtual HttpInterceptorId add_middleware(
        const std::string &plugin_name,
        HttpMiddlewareCallback callback,
        const std::string &name = "") = 0;

    /// 注册路由处理器
    /// 返回拦截器 ID, 0 表示失败
    virtual HttpInterceptorId add_route(
        const std::string &plugin_name,
        const std::string &path,
        HttpRouteCallback callback,
        const std::string &method = "") = 0;  ///< method 为空表示匹配所有方法

    /// 移除已注册的拦截器
    virtual bool remove(HttpInterceptorId id) = 0;

    /// 移除指定插件注册的所有拦截器
    virtual void remove_by_plugin(const std::string &plugin_name) = 0;

    /// 检查是否有可用的 HTTP 服务
    virtual bool is_available() const = 0;
};

} // namespace yuan::plugin

#endif
