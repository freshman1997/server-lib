#ifndef __RESPONSE_H__
#define __RESPONSE_H__

#include <memory>
#include <string>
#include <unordered_map>

#include "buffer/buffer.h"
#include "net/http/request.h"
#include "response_code.h"

namespace net::http
{
    class HttpRequestContext;
    
    class HttpResponse
    {
    public:
        HttpResponse(HttpRequestContext *context);

    public:
        void set_response_code(ResponseCode code)
        {
            respCode_ = code;
        }

        void set_response_version(HttpVersion version)
        {
            version_ = version;
        }

        void add_header(const std::string &k, const std::string &v)
        {
            if (k.empty()) {
                return;
            }

            headers_[k] = v;
        }

        void append_body(const char *data);
        
        void append_body(const std::string &data);

        std::shared_ptr<Buffer> get_buff()
        {
            return buffer_;
        }

        HttpRequestContext * get_context()
        {
            return context_;
        }

        void reset();

        void send();
        
    private:
        bool pack_response();

    private:
        ResponseCode respCode_ = ResponseCode::bad_request;
        HttpVersion version_ = HttpVersion::v_1_1;
        std::unordered_map<std::string, std::string> headers_;
        std::shared_ptr<Buffer> buffer_;
        HttpRequestContext *context_;
    };
}
#endif
