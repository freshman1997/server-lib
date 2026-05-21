#include "plugin_http_bridge.h"

#include "http/http_service.h"
#include "plugin_host_service.h"
#include "request.h"
#include "response.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace
{

std::string upper_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool method_matches(const std::string &expected, yuan::net::http::HttpRequest *req)
{
    if (expected.empty()) {
        return true;
    }
    return upper_copy(expected) == upper_copy(req ? req->get_raw_method() : "");
}

} // namespace

namespace yuan::server
{

bool install_plugin_http_bridge(yuan::app::PluginHostService &plugin_host, HttpService &http_service)
{
    plugin_host.set_http_server_accessor([&http_service]() -> void * {
        return &http_service.server();
    });

    plugin_host.set_http_installers(
        [&http_service](std::shared_ptr<yuan::plugin::HttpMiddlewareCallback> callback,
                        std::string name) -> bool {
            if (!callback) {
                return false;
            }

            http_service.server().use(
                [callback = std::move(callback)](yuan::net::http::HttpRequest *req,
                                                 yuan::net::http::HttpResponse *resp) {
                    auto cb = callback ? *callback : nullptr;
                    if (!cb) {
                        return yuan::net::http::MiddlewareResult::next;
                    }

                    const std::string path(req ? req->get_path() : std::string_view{});
                    const std::string method = req ? req->get_raw_method() : "";
                    return cb(path, method, req, resp)
                        ? yuan::net::http::MiddlewareResult::next
                        : yuan::net::http::MiddlewareResult::stop;
                },
                "plugin.middleware");
            return true;
        },
        [&http_service](std::shared_ptr<yuan::plugin::HttpRouteCallback> callback,
                        std::string route_path,
                        std::string route_method,
                        std::string name) -> bool {
            if (!callback || route_path.empty()) {
                return false;
            }

            http_service.server().use(
                [callback = std::move(callback),
                 route_path = std::move(route_path),
                 route_method = std::move(route_method)](
                    yuan::net::http::HttpRequest *req,
                    yuan::net::http::HttpResponse *resp) {
                    if (!req || std::string(req->get_path()) != route_path ||
                        !method_matches(route_method, req)) {
                        return yuan::net::http::MiddlewareResult::next;
                    }

                    auto cb = callback ? *callback : nullptr;
                    if (!cb) {
                        return yuan::net::http::MiddlewareResult::next;
                    }

                    cb(std::string(req->get_path()), req->get_raw_method(), req, resp);
                    return yuan::net::http::MiddlewareResult::stop;
                },
                "plugin.route");
            return true;
        });

    return true;
}

} // namespace yuan::server
