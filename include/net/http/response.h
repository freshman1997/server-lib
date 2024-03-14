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
        void set_response_code(response_code::ResponseCode code)
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

            size_ += k.size();
            size_ += v.size();
        }

        size_t get_size() const 
        {
            return size_;
        }

        void append_body(const char *data);
        
        void append_body(const std::string &data);

        std::shared_ptr<Buffer> get_buff()
        {
            return buffer_;
        }

        void send();
        
    private:
        bool pack_response();

        void pack_error_reponse();

    private:
        response_code::ResponseCode respCode_ = response_code::ResponseCode::bad_request;
        HttpVersion version_ = HttpVersion::v_1_1;
        std::unordered_map<std::string, std::string> headers_;
        size_t size_ = 0;
        std::shared_ptr<Buffer> buffer_;
        HttpRequestContext *context_;
    };
}
#endif
