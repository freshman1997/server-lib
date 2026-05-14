#ifndef __NET_WEBDAV_HANDLER_H__
#define __NET_WEBDAV_HANDLER_H__

#include "webdav_lock_manager.h"
#include "webdav_resource.h"

#include "common.h"

#include <memory>
#include <string>

namespace yuan::net::http
{
    class HttpRequest;
    class HttpResponse;
    class HttpServer;
}

namespace yuan::net::webdav
{
    struct WebDavHandlerConfig
    {
        std::string mount_path = "/";
        bool allow_infinite_depth = false;
        bool create_parent_on_put = true;
        std::uint64_t max_put_bytes = 1024ull * 1024ull * 1024ull;
        std::uint32_t default_lock_timeout_seconds = 3600;
        std::uint32_t max_lock_timeout_seconds = 86400;
    };

    class WebDavHandler
    {
    public:
        WebDavHandler(std::shared_ptr<WebDavResourceBackend> backend, WebDavHandlerConfig config = {});
        WebDavHandler(std::shared_ptr<WebDavResourceBackend> backend,
                      std::shared_ptr<WebDavLockManager> locks,
                      WebDavHandlerConfig config = {});

        void handle(http::HttpRequest *req, http::HttpResponse *resp);
        http::request_function as_handler();

    private:
        std::string href_from_request(const http::HttpRequest &req) const;
        std::string destination_from_request(const http::HttpRequest &req) const;
        bool check_write_lock(const std::string &href, const http::HttpRequest &req, http::HttpResponse *resp) const;
        void finish(http::HttpResponse *resp) const;

        void options(http::HttpRequest *req, http::HttpResponse *resp);
        void get_or_head(http::HttpRequest *req, http::HttpResponse *resp, bool head);
        void put(http::HttpRequest *req, http::HttpResponse *resp);
        void remove(http::HttpRequest *req, http::HttpResponse *resp);
        void mkcol(http::HttpRequest *req, http::HttpResponse *resp);
        void propfind(http::HttpRequest *req, http::HttpResponse *resp);
        void proppatch(http::HttpRequest *req, http::HttpResponse *resp);
        void copy_or_move(http::HttpRequest *req, http::HttpResponse *resp, bool move);
        void lock(http::HttpRequest *req, http::HttpResponse *resp);
        void unlock(http::HttpRequest *req, http::HttpResponse *resp);
        void report_or_search(http::HttpRequest *req, http::HttpResponse *resp);

        std::shared_ptr<WebDavResourceBackend> backend_;
        WebDavHandlerConfig config_;
        std::shared_ptr<WebDavLockManager> locks_;
    };

    void mount_webdav(http::HttpServer &server,
                      const std::string &mount_path,
                      std::shared_ptr<WebDavResourceBackend> backend,
                      WebDavHandlerConfig config = {});
}

#endif
