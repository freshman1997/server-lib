#ifndef __REQUEST_H__
#define __REQUEST_H__

#include <string>
#include <unordered_map>
#include <vector>

#include "buff/buffer.h"

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

    bool parse_method(Buffer &buff);
    bool parse_url(Buffer &buff);
    bool parse_version(Buffer &buff);
    bool parse_headers(Buffer &buff);

    bool parse_body(Buffer &buff);

private:
    HeaderState header_state = HeaderState::init;
    BodyState body_state = BodyState::init;
    HttpRequest *req = nullptr;
};

class HttpRequest
{
    friend class HttpRequestParser;
public:
    HttpRequest();

    HttpMethod get_method() const;
    HttpVersion get_version() const;
    bool header_exists(std::string &key) const;

    bool parse_header(Buffer &buff);    

private:
    HttpRequestParser parser;
    std::vector<std::string> url_domain;
    HttpMethod method = HttpMethod::invalid_;
    HttpVersion version = HttpVersion::invalid;
    std::unordered_map<std::string, std::string> request_params;
    std::unordered_map<std::string, std::string> headers;
};

class HttpRequestContext
{
public:

private:
    HttpRequest req;
};

#endif
