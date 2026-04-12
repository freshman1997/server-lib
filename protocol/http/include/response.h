#ifndef __RESPONSE_H__
#define __RESPONSE_H__
#include "packet.h"
#include "response_code.h"

#include <string>
#include <string_view>

namespace yuan::net::http
{
    class HttpSessionContext;
    class HttpResponseParser;
    
    class HttpResponse : public HttpPacket
    {
    public:
        HttpResponse(HttpSessionContext *context);
        ~HttpResponse();

        // 禁用拷贝
        HttpResponse(const HttpResponse&) = delete;
        HttpResponse& operator=(const HttpResponse&) = delete;

    public:
        virtual void reset();

        virtual bool pack_header(Connection *conn = nullptr);

        virtual PacketType get_packet_type() { return PacketType::response; }

    public:
        void set_response_code(ResponseCode code) { respCode_ = code; }

        ResponseCode get_response_code() const { return respCode_; }

        // append_body - 避免拷贝的版本
        void append_body(std::string_view data);
        
        // 原始兼容接口
        void append_body(const char *data);
        void append_body(const std::string &data);
        // move版本
        void append_body(std::string &&data);

        void process_error(ResponseCode errorCode = ResponseCode::internal_server_error);

        void dispatch_task();

        // JSON快捷方法
        void json(const std::string &json_str, ResponseCode code = ResponseCode::ok_);
        
        // 重定向
        void redirect(const std::string &url, ResponseCode code = ResponseCode::found);

        // 设置Cookie
        void set_cookie(const std::string &name, const std::string &value,
                        int64_t max_age = -1, const std::string &path = "/",
                        const std::string &domain = "", bool http_only = true,
                        bool secure = false, const std::string &same_site = "Lax");

        // SSE相关
        void set_sse();
        bool is_sse() const { return is_sse_; }
        void send_sse_event(const std::string &event, const std::string &data, const std::string &id = "");

    private:
        ResponseCode respCode_ = ResponseCode::bad_request;
        bool is_sse_ = false;
        uint64_t sse_event_count_ = 0;
    };
}
#endif
