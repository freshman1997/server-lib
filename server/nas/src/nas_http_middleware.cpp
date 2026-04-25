#include "nas/nas_http_middleware.h"

#include "header_key.h"
#include "request.h"
#include "response.h"

namespace yuan::server::nas
{
    NasHttpAuthMiddleware::NasHttpAuthMiddleware(std::shared_ptr<NasAuthService> auth, NasHttpAuthOptions options)
        : auth_(std::move(auth)), options_(std::move(options))
    {
    }

    yuan::net::http::MiddlewareResult NasHttpAuthMiddleware::process(yuan::net::http::HttpRequest *req,
                                                                     yuan::net::http::HttpResponse *resp)
    {
        if (!req || !resp) {
            return yuan::net::http::MiddlewareResult::forbidden;
        }

        if (req->is_options() || (options_.allow_anonymous_read && is_read_method(*req))) {
            return yuan::net::http::MiddlewareResult::next;
        }

        const std::string *auth_header = req->get_header(yuan::net::http::http_header_key::authorization);
        if (!auth_ || !auth_header || auth_header->empty()) {
            resp->add_header("WWW-Authenticate", "Basic realm=\"" + options_.realm + "\"");
            return yuan::net::http::MiddlewareResult::unauthorized;
        }

        const auto auth_result = auth_->authenticate_basic_header(*auth_header);
        if (!auth_result.authenticated) {
            resp->add_header("WWW-Authenticate", "Basic realm=\"" + options_.realm + "\"");
            return yuan::net::http::MiddlewareResult::unauthorized;
        }

        req->add_header("x-nas-user-id", auth_result.user.id);
        req->add_header("x-nas-username", auth_result.user.username);
        req->add_header("x-nas-admin", auth_result.user.admin ? "1" : "0");
        return yuan::net::http::MiddlewareResult::next;
    }

    const char *NasHttpAuthMiddleware::name() const
    {
        return "NasHttpAuth";
    }

    bool NasHttpAuthMiddleware::is_read_method(const yuan::net::http::HttpRequest &req)
    {
        return req.is_get() || req.is_head() || req.is_propfind() || req.is_report() || req.is_search();
    }

    std::shared_ptr<yuan::net::http::HttpMiddleware> nas_basic_auth_middleware(
        std::shared_ptr<NasAuthService> auth,
        NasHttpAuthOptions options)
    {
        return std::make_shared<NasHttpAuthMiddleware>(std::move(auth), std::move(options));
    }
}
