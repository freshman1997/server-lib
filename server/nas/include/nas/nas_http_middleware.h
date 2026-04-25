#ifndef __YUAN_SERVER_NAS_HTTP_MIDDLEWARE_H__
#define __YUAN_SERVER_NAS_HTTP_MIDDLEWARE_H__

#include "nas/nas_auth_service.h"
#include "nas/nas_types.h"

#include "middleware.h"

#include <memory>
#include <string>

namespace yuan::server::nas
{
    struct NasHttpAuthOptions
    {
        std::string realm = "Yuan NAS";
        bool allow_anonymous_read = false;
    };

    class NasHttpAuthMiddleware final : public yuan::net::http::HttpMiddleware
    {
    public:
        NasHttpAuthMiddleware(std::shared_ptr<NasAuthService> auth, NasHttpAuthOptions options = {});

        yuan::net::http::MiddlewareResult process(yuan::net::http::HttpRequest *req,
                                                  yuan::net::http::HttpResponse *resp) override;
        const char *name() const override;

        static bool is_read_method(const yuan::net::http::HttpRequest &req);

    private:
        std::shared_ptr<NasAuthService> auth_;
        NasHttpAuthOptions options_;
    };

    std::shared_ptr<yuan::net::http::HttpMiddleware> nas_basic_auth_middleware(
        std::shared_ptr<NasAuthService> auth,
        NasHttpAuthOptions options = {});
}

#endif
