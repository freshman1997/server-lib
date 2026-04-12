#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "common.h"
#include "define/upload.h"
#include "middleware.h"
#include "net/handler/connection_handler.h"
#include "net/secuity/ssl_module.h"
#include "request_dispatcher.h"
#include "thread/thread_pool.h"
#include "timer/timer_manager.h"

namespace yuan::net
{
    class Socket;
    class Poller;
    class EventLoop;
    class StreamAcceptor;
}

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
        size_t max_body_size = 0;
        std::string server_name = "YuanServer/1.0";
    };

    class HttpServer : public ConnectionHandler
    {
    public:
        HttpServer();
        explicit HttpServer(const HttpServerConfig &config);
        ~HttpServer();

        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

    public:
        void on_connected(Connection *conn) override;
        void on_error(Connection *conn) override;
        void on_read(Connection *conn) override;
        void on_write(Connection *conn) override;
        void on_close(Connection *conn) override;

    public:
        bool init(int port);
        void serve();
        void stop();

        EventLoop * get_event_loop() { return event_loop_; }
        timer::TimerManager * get_timer_manager() { return timer_manager_; }

    public:
        void on(const std::string &url, request_function func, bool is_prefix = false);
        void on(const std::string &url, request_function func,
                std::shared_ptr<MiddlewarePipeline> pipeline, bool is_prefix = false);
        void use(std::shared_ptr<HttpMiddleware> middleware);
        void use(middleware_function fn, const char *name = "anonymous");
        const MiddlewarePipeline& global_middleware() const { return global_pipeline_; }

    public:
        const HttpServerConfig& config() const { return config_; }

    private:
        static Poller * create_default_poller();
        bool init_runtime(int port);
        bool init_ssl_if_needed();
        bool init_http_features();
        void register_builtin_routes();
        bool init_proxy_if_needed();
        bool parse_request(HttpSessionContext *context);
        bool dispatch_request(HttpSessionContext *context);
        void finalize_request(uint64_t session_id, HttpSession *session, HttpSessionContext *context);
        void bind_event_loop(EventLoop *loop);
        void cleanup_runtime();
        void free_session(Connection *conn);
        void load_static_paths();
        static void icon(HttpRequest *req, HttpResponse *resp);
        void serve_static(HttpRequest *req, HttpResponse *resp);
        bool resolve_static_request(
            const std::string &url,
            std::string &prefix,
            std::string &path_prefix,
            std::string &file_relative_path,
            HttpResponse *resp);
        bool serve_embedded_static_page(const std::string &file_relative_path, HttpResponse *resp);
        void serve_static_file(
            HttpRequest *req,
            HttpResponse *resp,
            const std::string &url,
            const std::string &file_relative_path,
            const std::string &path_prefix);
        static void serve_download(const std::string &filePath, const std::string &ext, HttpResponse *resp);
        static void serve_list_files(const std::string &prefix, const std::string &filePath, HttpResponse *resp);
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

    private:
        bool quit_ = false;
        Poller *poller_ = nullptr;
        StreamAcceptor *acceptor_ = nullptr;
        EventLoop *event_loop_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unordered_map<uint64_t, HttpSession *> sessions_;
        HttpRequestDispatcher dispatcher_;
        std::unordered_map<std::string, std::string> static_paths_;
        std::set<std::string> play_types_;
        HttpProxy *proxy_ = nullptr;
        std::unordered_map<std::string, UploadFileMapping> uploaded_chunks_;
        std::unique_ptr<thread::ThreadPool> thread_pool_;
        HttpServerConfig config_;
        MiddlewarePipeline global_pipeline_;
    };
}
#endif










