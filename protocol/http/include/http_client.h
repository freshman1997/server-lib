#ifndef __NET_HTTP_CLIETNT_H__
#define __NET_HTTP_CLIETNT_H__
#include "coroutine/task.h"
#include "content_type.h"
#include "net/async/async_client_session.h"
#include "net/runtime/network_runtime.h"
#include "net/secuity/ssl_module.h"
#include "net/socket/inet_address.h"
#include "response_code.h"
#include "common.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace yuan::net::http
{
    typedef std::function<void(HttpRequest * req)> connected_callback;

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

    class HttpClient
    {
    public:
        HttpClient();
        ~HttpClient();

    public:
        bool query(const std::string &url);

        bool connect(connected_callback ccb, request_function rcb);

        yuan::coroutine::Task<HttpResponse *> connect_async(
            NetworkRuntime::RuntimeView runtime,
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponse *> connect_async(
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponseSnapshot> connect_snapshot_async(
            NetworkRuntime::RuntimeView runtime,
            connected_callback ccb,
            uint32_t timeout_ms = 0);

        yuan::coroutine::Task<HttpResponseSnapshot> connect_snapshot_async(
            connected_callback ccb,
            uint32_t timeout_ms = 0);

    private:
        static HttpResponseSnapshot snapshot_response(HttpResponse *response);

        yuan::coroutine::Task<HttpResponse *> do_connect_async(
            yuan::coroutine::RuntimeView runtime,
            connected_callback ccb,
            uint32_t timeout_ms);

    private:
        net::AsyncClientSession session_;
        std::unique_ptr<net::NetworkRuntime> owned_runtime_;
        int port_;
        std::string host_name_;
        std::shared_ptr<SSLModule> ssl_module_;
        HttpSession *last_session_ = nullptr;
    };
}
#endif
