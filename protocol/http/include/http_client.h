#ifndef __NET_HTTP_CLIETNT_H__
#define __NET_HTTP_CLIETNT_H__
#include "coroutine/completion_event.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "content_type.h"
#include "event/event_loop.h"
#include "net/handler/connection_handler.h"
#include "net/secuity/ssl_module.h"
#include "net/socket/inet_address.h"
#include "response_code.h"
#include "timer/timer.h"
#include "common.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace yuan::net::http 
{
    typedef std::function<void (HttpRequest *req)> connected_callback;

    struct HttpResponseSnapshot
    {
        bool good = false;
        ResponseCode response_code = ResponseCode::bad_request;
        ContentType content_type = ContentType::not_support;
        bool downloading = false;
        std::string original_file_name;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    class HttpClient : public ConnectionHandler
    {
        struct RequestState
        {
            ~RequestState();

            HttpSession *session = nullptr;
            request_function rcb = nullptr;
            connected_callback ccb = nullptr;
            net::EventLoop *event_loop = nullptr;
            timer::TimerManager *timer_manager = nullptr;
            timer::Timer *conn_timer = nullptr;
            bool coroutine_waiting_response = false;
            bool request_completed = false;
            bool request_failed = false;
            yuan::coroutine::CompletionEvent completion_event;
        };

    public:
        HttpClient();
        ~HttpClient();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool query(const std::string &url);

        bool connect(connected_callback ccb, request_function rcb);

        yuan::coroutine::Task<HttpResponse *> connect_async(
            yuan::coroutine::RuntimeView runtime,
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponse *> connect_async(
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponseSnapshot> connect_snapshot_async(
            yuan::coroutine::RuntimeView runtime,
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponseSnapshot> connect_snapshot_async(
            connected_callback ccb,
            uint32_t timeout_ms = 0);
    
        void on_timer(timer::Timer *timer);

    private:
        net::Connection *create_connection();
        void bind_runtime(
            net::Connection *conn,
            net::EventLoop *event_loop,
            timer::TimerManager *timer_manager,
            connected_callback ccb,
            request_function rcb);
        void start_request_state();
        RequestState *request_state() const;
        void fail_or_complete_request(bool failed);
        void clear_runtime_binding();
        static HttpResponseSnapshot snapshot_response(HttpResponse *response);
        void exit();

    private:
        int port_;
        std::string host_name_;
        std::shared_ptr<SSLModule> ssl_module_;
        bool coroutine_request_mode_;
        std::unique_ptr<RequestState> request_state_;
    };
}

#endif
