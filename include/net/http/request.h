#ifndef __REQUEST_H__
#define __REQUEST_H__

#include <string>
#include <unordered_map>
#include <vector>

#include "buffer/buffer.h"
#include "net/http/content_type.h"
#include "net/http/response_code.h"
#include "net/http/content/types.h"

namespace net::http 
{
    class HttpRequestContext;

    enum class HttpMethod
    {
        invalid_ = 0,
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

    enum class HttpVersion : char
    {
        invalid = -1,
        v_1_0,
        v_1_1,
        v_2_0,
        v_3_0,
    };

    class HttpRequest;
    class HttpRequestParser
    {
        friend class HttpRequest;
    public:
        enum class HeaderState
        {
            init = 0,
            metohd,                 // 方法
            method_gap,             // 方法接下来的空格
            url,                    // url
            url_gap,                // url 接下来的空格
            version,                // 版本信息
            version_newline,        // 换行
            header_key,             // 头部key
            header_value,           // 值
            header_newline,         // 换行
            header_end_lines,       // 最后换行
        };

        enum class BodyState
        {
            init = 0,
            partial,
            fully,
        };

        HttpRequestParser() {}
        HttpRequestParser(HttpRequest *req_) : req(req_) 
        {}

        void set_req(HttpRequest *req_)
        {
            this->req = req_;
        }

        void reset() 
        {
            header_state = HeaderState::init;
            body_state = BodyState::init;
        }

        int parse(Buffer &buff);

        bool done() const
        {
            return is_header_done() && is_body_done();
        }

    private:
        bool parse_method(Buffer &buff);
        bool parse_url(Buffer &buff);
        bool parse_version(Buffer &buff);
        bool parse_header_keys(Buffer &buff);
        bool parse_new_line(Buffer &buff);

        bool parse_header(Buffer &buff);
        int parse_body(Buffer &buff, uint32_t length);
    
        uint32_t get_body_length();

        bool is_header_done() const 
        {
            return header_state == HeaderState::header_end_lines;
        }

        bool is_body_done() const 
        {
            return body_state == BodyState::fully;
        }

    private:
        HeaderState header_state = HeaderState::init;
        BodyState body_state = BodyState::init;
        HttpRequest *req = nullptr;
    };

    class HttpRequest
    {
        friend class HttpRequestParser;
    public:
        HttpRequest(HttpRequestContext *context_);

    public:
        HttpMethod get_method() const;
        std::string get_raw_method() const;

        HttpVersion get_version() const;
        std::string get_raw_version() const;

        bool header_exists(const std::string &key) const;

        const std::string * get_header(const std::string &key) const;

        bool parse(Buffer &buff);

        const std::unordered_map<std::string, std::string> & get_request_params() const
        {
            return request_params_;
        }

        const std::vector<std::string> & get_url_domain() const
        {
            return url_domain_;
        }

        const std::string & get_raw_url() const 
        {
            return url_;
        }

        const char * body_begin();

        const char * body_end();

        void read_body_done();

        const std::unordered_map<std::string, std::string> & get_content_type_extra() const 
        {
            return content_type_extra_;
        }

        void set_body_content(const Content &content)
        {
            body_content_ = content;
        }

        const Content & get_body_content() const
        {
            return body_content_;
        }

    public:
        bool is_ok() const 
        {
            return parser_.done();
        }

        bool good() const 
        {
            return is_good_;
        }

        ResponseCode get_error_code() const
        {
            return error_code_;
        }

        HttpRequestContext * get_context()
        {
            return context_;
        }

        void reset();

    private:
        bool parse_content_type();

        bool parse_content();

    private:
        HttpRequestContext *context_;
        bool is_good_;

    private:
        uint32_t body_length_;
        HttpMethod method_;
        HttpVersion version_;
        content_type content_type_;
        ResponseCode error_code_;
        HttpRequestParser parser_;
        std::string url_;
        std::vector<std::string> url_domain_;
        std::unordered_map<std::string, std::string> request_params_;
        std::unordered_map<std::string, std::string> headers_;
        std::unordered_map<std::string, std::string> content_type_extra_;
        Content body_content_;
    };
}

#endif
