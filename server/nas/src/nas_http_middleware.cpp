#include "nas/nas_http_middleware.h"

#include "header_key.h"
#include "request.h"
#include "response.h"

#include <cctype>

namespace yuan::server::nas
{
    namespace
    {
        constexpr const char *kHdrNasUserId = "x-nas-user-id";
        constexpr const char *kHdrNasUsername = "x-nas-username";
        constexpr const char *kHdrNasAdmin = "x-nas-admin";

        bool contains_ci(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty() || haystack.size() < needle.size()) {
                return false;
            }
            for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
                bool match = true;
                for (std::size_t j = 0; j < needle.size(); ++j) {
                    const auto a = static_cast<unsigned char>(haystack[i + j]);
                    const auto b = static_cast<unsigned char>(needle[j]);
                    if (std::tolower(a) != std::tolower(b)) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return true;
                }
            }
            return false;
        }

        bool is_browser_request(const yuan::net::http::HttpRequest &req)
        {
            const auto *ua = req.get_header("user-agent");
            if (!ua || ua->empty()) {
                return false;
            }
            return contains_ci(*ua, "mozilla") ||
                   contains_ci(*ua, "chrome") ||
                   contains_ci(*ua, "safari") ||
                   contains_ci(*ua, "firefox") ||
                   contains_ci(*ua, "edge") ||
                   contains_ci(*ua, "edg/");
        }

        std::string path_without_query(std::string path)
        {
            const auto hash = path.find('#');
            if (hash != std::string::npos) {
                path.resize(hash);
            }
            const auto query = path.find('?');
            if (query != std::string::npos) {
                path.resize(query);
            }
            return path;
        }

        bool is_unreserved(unsigned char ch)
        {
            return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        }

        std::string percent_encode(std::string_view value)
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(value.size());
            for (const auto ch : value) {
                const auto byte = static_cast<unsigned char>(ch);
                if (is_unreserved(byte) || byte == '/') {
                    out.push_back(static_cast<char>(byte));
                    continue;
                }
                out.push_back('%');
                out.push_back(kHex[(byte >> 4) & 0x0F]);
                out.push_back(kHex[byte & 0x0F]);
            }
            return out;
        }

        bool should_redirect_browser_to_login(const yuan::net::http::HttpRequest &req,
                                              const NasHttpAuthOptions &options)
        {
            if (!req.is_get() && !req.is_head()) {
                return false;
            }
            if (!is_browser_request(req)) {
                return false;
            }
            const auto raw_path = path_without_query(req.get_raw_url());
            return raw_path == options.mount_path ||
                   raw_path.rfind(options.mount_path + "/", 0) == 0;
        }
    }

    NasHttpAuthMiddleware::NasHttpAuthMiddleware(std::shared_ptr<NasAuthService> auth, NasHttpAuthOptions options)
        : auth_(std::move(auth)),
          options_(std::move(options)),
          adapter_(options_.mount_path)
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

        if (req->get_raw_url().size() > options_.max_request_path_bytes) {
            resp->json("{\"error\":\"request path too long\"}", yuan::net::http::ResponseCode::uri_too_long);
            resp->send();
            return yuan::net::http::MiddlewareResult::stop;
        }

        const std::string *auth_header = req->get_header(yuan::net::http::http_header_key::authorization);
        if (auth_header && auth_header->size() > options_.max_authorization_header_bytes) {
            resp->json("{\"error\":\"authorization header too large\"}", yuan::net::http::ResponseCode::request_header_fields_too_large);
            resp->send();
            return yuan::net::http::MiddlewareResult::stop;
        }
        if (!auth_ || !auth_header || auth_header->empty()) {
            if (should_redirect_browser_to_login(*req, options_)) {
                const auto target = path_without_query(req->get_raw_url());
                resp->redirect("/nas/files?path=" + percent_encode(target), yuan::net::http::ResponseCode::found);
                resp->add_header("Content-Length", "0");
                resp->send();
                return yuan::net::http::MiddlewareResult::stop;
            }
            resp->add_header("WWW-Authenticate", "Basic realm=\"" + options_.realm + "\"");
            return yuan::net::http::MiddlewareResult::unauthorized;
        }

        const auto auth_result = auth_->authenticate_basic_header(*auth_header);
        if (!auth_result.authenticated) {
            resp->add_header("WWW-Authenticate", "Basic realm=\"" + options_.realm + "\"");
            return yuan::net::http::MiddlewareResult::unauthorized;
        }

        req->add_header(kHdrNasUserId, auth_result.user.id);
        req->add_header(kHdrNasUsername, auth_result.user.username);
        req->add_header(kHdrNasAdmin, auth_result.user.admin ? "1" : "0");

        if (!enforce_share_acl(req, resp)) {
            return yuan::net::http::MiddlewareResult::forbidden;
        }

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

    bool NasHttpAuthMiddleware::enforce_share_acl(yuan::net::http::HttpRequest *req,
                                                  yuan::net::http::HttpResponse *resp) const
    {
        if (!req || !resp || !options_.share_manager || !options_.metadata) {
            return true;
        }

        auto route = adapter_.parse_route(req->get_raw_url());
        if (!route) {
            return true;
        }

        auto share = options_.share_manager->find_by_name(route->share_name);
        if (!share) {
            resp->json("{\"error\":\"share not found\"}", yuan::net::http::ResponseCode::not_found);
            resp->send();
            return false;
        }

        const auto *username = req->get_header(kHdrNasUsername);
        if (!username || username->empty()) {
            resp->json("{\"error\":\"unauthorized\"}", yuan::net::http::ResponseCode::unauthorized);
            resp->send();
            return false;
        }

        auto user = options_.metadata->find_user_by_name(*username);
        if (!user) {
            resp->json("{\"error\":\"unauthorized\"}", yuan::net::http::ResponseCode::unauthorized);
            resp->send();
            return false;
        }

        const auto required = NasPermissionService::required_for_webdav_request(*req);
        if (!permission_service_.allowed(*share, *user, required)) {
            resp->json("{\"error\":\"forbidden\"}", yuan::net::http::ResponseCode::forbidden);
            resp->send();
            return false;
        }

        return true;
    }

    std::shared_ptr<yuan::net::http::HttpMiddleware> nas_basic_auth_middleware(
        std::shared_ptr<NasAuthService> auth,
        NasHttpAuthOptions options)
    {
        return std::make_shared<NasHttpAuthMiddleware>(std::move(auth), std::move(options));
    }
}
