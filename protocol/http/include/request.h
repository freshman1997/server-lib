#ifndef __NET_HTTP_REQUEST_H__
#define __NET_HTTP_REQUEST_H__

#include <string>
#include <vector>

#include "packet.h"

namespace yuan::net::http 
{ 
    enum class HttpMethod : char
    {
        invalid_ = -1,
        get_,
        post_,
        put_,
        delete_,
        options_,
        head_,
        comment_,
        trace_,
        patch_,
    };

    class HttpRequest : public HttpPacket
    {
        friend class HttpRequestParser;
    public:
        explicit HttpRequest(HttpSessionContext *context_);
        ~HttpRequest() override;

    public:
        virtual void reset();

        virtual bool pack_header(Connection *conn = nullptr);

        virtual PacketType get_packet_type()
        {
            return PacketType::request;
        }

    public:
        HttpMethod get_method() const;
        std::string get_raw_method() const;

        void set_method(HttpMethod method)
        {
            method_ = method;
        }

        const std::vector<std::string> & get_url_domain() const
        {
            return url_domain_;
        }

        const std::string & get_raw_url() const 
        {
            return url_;
        }

        void set_raw_url(const std::string &url)
        {
            url_ = url;
        }

        std::string get_last_uri();

    private:
        HttpMethod method_;
        std::string url_;
        std::vector<std::string> url_domain_;
    };
}

#endif
