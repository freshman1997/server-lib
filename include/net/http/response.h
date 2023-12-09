#ifndef __RESPONSE_H__
#define __RESPONSE_H__

#include <memory>
#include <string>
#include <unordered_map>

#include "net/channel/channel.h"
#include "net/http/request.h"
#include "response_code.h"

namespace net::http
{
    class HttpResponse
    {
    public:
        HttpResponse(std::shared_ptr<Channel> channel);

    public:
        void set_response_code(response_code::ResponseCode code)
        {
            respCode_ = code;
        }

        void set_response_version(HttpVersion version)
        {
            version_ = version;
        }

        template<class T>
        void add_header(const std::string &k, T v)
        {
            if (k.empty()) {
                return;
            }

            const std::string &val = std::to_string(v);
            headers_[k] = val;

            size += k.size();
            size += val.size();
        }

        size_t get_size() const 
        {
            return size;
        }

    private:
        response_code::ResponseCode respCode_ = response_code::ResponseCode::invalid;
        HttpVersion version_ = HttpVersion::invalid;
        std::unordered_map<std::string, std::string> headers_;
        std::shared_ptr<Channel> channel_;
        size_t size = 0;
    };
}
#endif
