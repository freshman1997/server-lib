#ifndef __REQUEST_H__
#define __REQUEST_H__

#include <string>
#include <unordered_map>
class Buffer;

enum class HttpMethod
{
    invalid_ = 0,
    get_,
    post_,
    put_,
    delete_,
    option_,
};

class HttpRequestParser
{
public:
    enum class HeaderState
    {
        init = 0,
        metohd,                 // 方法
        metohd_gap,             // 方法接下来的空格
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

    bool parse_method(Buffer *buff);
    bool parse_url(Buffer *buff);
    bool parse_version(Buffer *buff);
    bool parse_headers(Buffer *buff);

    bool parse_body(Buffer *buff);

public:
    HeaderState header_state = HeaderState::init;
    BodyState body_state = BodyState::init;
};

class HttpRequest
{

public:
    HttpMethod method = HttpMethod::invalid_;
    HttpRequestParser parser;
    std::unordered_map<std::string, std::string> headers;
};

class HttpRequestContext
{
public:

private:
    HttpRequest req;
};

#endif
