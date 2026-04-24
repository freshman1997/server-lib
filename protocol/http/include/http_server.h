#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "define/upload.h"
#include "middleware.h"
#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"
#include "net/secuity/ssl_module.h"
#include "request_dispatcher.h"
#include "thread/thread_pool.h"

#include <functional>

namespace yuan::net::http
{
    class HttpProxy;
    class HttpSession;
    class HttpSessionContext;
    struct FormDataContent;
    struct FormDataFileItem;

    struct HttpServerConfig
    {
        int thread_pool_size = 1;
        bool enable_cors = true;
        bool enable_keep_alive = true;
        bool enable_http2 = false;
        bool enable_http3 = false;
        size_t max_body_size = 0;
        std::string server_name = "YuanServer/1.0";
    };

    struct StaticMountOptions
    {
        bool auto_index = true;
        bool enable_legacy_embedded_pages = true;
        bool enable_range = true;
        std::vector<std::string> index_files{ "index.html", "index.htm" };
        std::vector<std::string> try_files;
        std::unordered_map<int, std::string> error_pages;
    };

    struct StaticMount
    {
        std::string prefix;
        std::string root;
        StaticMountOptions options;
    };

    class HttpServer
    {
    public:
        HttpServer();
        explicit HttpServer(const HttpServerConfig &config);
        ~HttpServer();

        HttpServer(const HttpServer &) = delete;
        HttpServer &operator=(const HttpServer &) = delete;

    public:
        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        void serve();
        void stop();

        NetworkRuntime *runtime() const noexcept
        {
            return listener_.runtime();
        }

        HttpProxy *get_proxy() const noexcept
        {
            return proxy_ ? &*proxy_ : nullptr;
        }

        HttpProxy *ensure_proxy();
        void mount_static(const std::string &url_prefix, const std::string &root, StaticMountOptions options = {});

    public:
        void on(const std::string &url, request_function func, bool is_prefix = false);
        void on(const std::string &url, request_function func,
                std::shared_ptr<MiddlewarePipeline> pipeline, bool is_prefix = false);
        void use(std::shared_ptr<HttpMiddleware> middleware);
        void use(middleware_function fn, const char *name = "anonymous");

        using WsProxyHandler = std::function<coroutine::Task<void>(
            net::AsyncConnectionContext, const std::string &, const std::string &,
            const std::string &, const std::string &, ::yuan::buffer::ByteBuffer)>;
        using AccessLogHook = std::function<void(HttpRequest *, HttpResponse *, uint64_t)>;

        void set_ws_proxy_handler(WsProxyHandler handler)
        {
            ws_proxy_handler_ = std::move(handler);
        }
        void set_access_log_hook(AccessLogHook hook)
        {
            access_log_hook_ = std::move(hook);
        }
        const MiddlewarePipeline &global_middleware() const
        {
            return global_pipeline_;
        }

    public:
        const HttpServerConfig &config() const
        {
            return config_;
        }

    private:
        bool init_ssl_if_needed();
        bool init_http_features();
        void refresh_h2_dispatch_paths();
        void register_builtin_routes();
        bool init_proxy_if_needed();
        void cleanup_stale_upload_sessions();
        bool parse_request(HttpSessionContext *context);
        bool parse_request(HttpSessionContext *context, const ::yuan::buffer::ByteBuffer &data);
        bool validate_request_version(HttpSessionContext *context);
        bool is_h2_dispatch_path(std::string_view path) const;
        bool dispatch_h2_context(HttpSessionContext *context);
        bool dispatch_request(HttpSessionContext *context);
        void finalize_request(uint64_t session_id, HttpSession *session, HttpSessionContext *context);
        void load_static_paths();
        static void icon(HttpRequest *req, HttpResponse *resp);
        void serve_static(HttpRequest *req, HttpResponse *resp);
        bool resolve_static_request(
            const std::string &request_path,
            const StaticMount *&mount,
            std::string &file_relative_path,
            HttpResponse *resp);
        static std::string make_weak_etag(const std::filesystem::path &path, std::size_t size, std::time_t modified_at);
        static std::string format_http_date(std::time_t ts);
        static bool parse_http_date(std::string_view text, std::time_t &out);
        static bool should_return_not_modified(HttpRequest *req, const std::string &etag, std::time_t modified_at);
        static bool maybe_compress_static_response(HttpRequest *req,
                                                   HttpResponse *resp,
                                                   const std::string &content_type,
                                                   const std::string &source_path,
                                                   std::size_t source_length);
        bool serve_embedded_static_page(const std::string &file_relative_path, HttpResponse *resp, const StaticMountOptions &options);
        void serve_static_file(
            HttpRequest *req,
            HttpResponse *resp,
            const StaticMount &mount,
            const std::string &file_relative_path,
            const std::string &full_path);
        static void serve_download(const std::string &filePath, const std::string &ext, HttpResponse *resp);
        static void serve_list_files(const std::string &prefix, const std::string &filePath, const std::string &request_path, HttpResponse *resp, bool as_json = false);
        void reload_config(HttpRequest *req, HttpResponse *resp);
        void serve_upload(HttpRequest *req, HttpResponse *resp);
        bool parse_upload_request(
            HttpRequest *req,
            HttpResponse *resp,
            FormDataContent *&form,
            std::string &upload_id,
            int &chunk_index,
            std::string &filename,
            FormDataFileItem *&file_item,
            uint64_t &chunk_size,
            int &total_chunks,
            uint64_t &file_size);
        bool find_or_create_upload_session(
            const std::string &upload_id,
            int chunk_index,
            const std::string &filename,
            int total_chunks,
            uint64_t file_size,
            HttpResponse *resp,
            std::unordered_map<std::string, UploadFileMapping>::iterator &session_it);
        bool store_upload_chunk(
            HttpRequest *req,
            HttpResponse *resp,
            const std::string &upload_id,
            int chunk_index,
            FormDataFileItem *file_item,
            uint64_t chunk_size,
            UploadSession &session,
            UploadSession &session_snapshot,
            int &received_count);
        void finalize_upload_chunk(
            HttpRequest *req,
            int chunk_index,
            FormDataFileItem *file_item,
            const UploadSession &session_snapshot,
            int received_count);
        void handle_options_preflight(HttpRequest *req, HttpResponse *resp);

        yuan::coroutine::Task<void> handle_connection(net::AsyncConnectionContext ctx);

    private:
        net::AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unordered_map<uint64_t, std::unique_ptr<HttpSession> > sessions_;
        HttpRequestDispatcher dispatcher_;
        base::CompressTrie static_mount_trie_;
        std::unordered_map<std::string, StaticMount> static_mounts_;
        std::set<std::string> play_types_;
        std::unique_ptr<HttpProxy> proxy_;
        WsProxyHandler ws_proxy_handler_;
        AccessLogHook access_log_hook_;
        std::unordered_map<std::string, UploadFileMapping> uploaded_chunks_;
        uint64_t upload_cleanup_last_ms_ = 0;
        std::unique_ptr<thread::ThreadPool> thread_pool_;
        HttpServerConfig config_;
        MiddlewarePipeline global_pipeline_;
        std::unordered_set<std::string> h2_dispatch_paths_;
    };
}
#endif
