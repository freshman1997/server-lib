#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "net/handler/connection_handler.h"
#include "common.h"
#include "context.h"
#include "net/secuity/ssl_module.h"
#include "request_dispatcher.h"
#include "timer/timer_manager.h"

namespace yuan::net
{
    class Socket;
    class Poller;
    class EventLoop;
    class Acceptor;
}

namespace yuan::net::http 
{
    class HttpProxy;

    class HttpServer : public ConnectionHandler
    {
        enum State
        {
            invalid = -1,
            create_socket,
            bind_address,
            listen_address,
            create_acceptor,
            create_poller,
            create_event_loop,
            success
        };        

    public:
        HttpServer();
        ~HttpServer();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool init(int port);

        void serve();

        void stop();

        EventLoop * get_event_loop()
        {
            return event_loop_;
        }

        timer::TimerManager * get_timer_manager()
        {
            return timer_manager_;
        }

    public:
        void on(const std::string &url, request_function func, bool is_prefix = false);

    private:
        void free_session(Connection *conn);

        void load_static_paths();

        static void icon(net::http::HttpRequest *req, net::http::HttpResponse *resp);

        void serve_static(HttpRequest *req, HttpResponse *resp);

        static void serve_download(const std::string &filePath, const std::string &ext, HttpResponse *resp);

        static void serve_list_files(const std::string &relPath, const std::string &filePath, HttpResponse *resp);

        void reload_config(HttpRequest *req, HttpResponse *resp);

        void serve_upload(HttpRequest *req, HttpResponse *resp);

    private:
        struct UploadChunk
        {
            int idx_;
            uint64_t chunk_size_;
            std::string tmp_file_;
        };

        struct UploadFileMapping
        {
            std::string origin_file_name_;
            uint64_t file_size_;
            int total_chunks_;
            std::unordered_map<int, UploadChunk> chunks_;
        };

    private:
        bool quit_;
        State state_;
        Poller *poller_;
        Acceptor *acceptor_;
        EventLoop *event_loop_;
        timer::TimerManager *timer_manager_;
        std::shared_ptr<SSLModule> ssl_module_;
        std::unordered_map<uint64_t, HttpSession *> sessions_;
        HttpRequestDispatcher dispatcher_;
        std::unordered_map<std::string, std::string> static_paths_;
        std::set<std::string> play_types_;
        HttpProxy *proxy_;
        std::unordered_map<std::string, UploadFileMapping> uploaded_chunks_;
    };
}

#endif
