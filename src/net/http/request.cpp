#include "net/base/connection/connection.h"

#include "net/http/request.h"
#include "net/http/content/content_parser_factory.h"
#include "net/http/header_key.h"
#include "net/http/ops/option.h"
#include "net/http/response_code.h"
#include "net/http/url.h"
#include "net/http/request_context.h"
#include "net/http/content_type.h"
#include "singleton/singleton.h"

#include <iostream>

namespace net::http 
{
    static const char* http_method_descs[9] = {
        "get",
        "post",
        "put",
        "delete",
        "option",
        "head",
        "comment",
        "trace",
        "patch",
    };

    static const char* http_version_descs[4] = {
        "1.0",
        "1.1",
        "2.0",
        "3.0"
    };

    bool HttpRequestParser::parse_method(Buffer &buff)
    {
        if (buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::metohd;
        char ch = std::tolower(buff.read_int8());
        std::string method;
        method.push_back(ch);
        switch (ch) {
            case 'g': {
                for (int i = 0; i < 2 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "get") {
                    return false;
                }

                req->method_ = HttpMethod::get_;
                break;
            }
            case 'p': {
                for (int i = 0; i < 2 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method == "put") {
                    req->method_ = HttpMethod::put_;
                    break;
                }

                method.push_back(std::tolower(buff.read_int8()));
                if (method == "post") {
                    req->method_ = HttpMethod::post_;
                    break;
                }

                method.push_back(std::tolower(buff.read_int8()));
                if (method != "patch") {
                    return false;
                }

                req->method_ = HttpMethod::post_;
                break;
            }
            case 'd': {
                for (int i = 0; i < 6 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "delete") {
                    return false;
                }

                req->method_ = HttpMethod::delete_;
                break;
            }
            case 'o': {
                for (int i = 0; i < 7 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "options") {
                    return false;
                }

                req->method_ = HttpMethod::options_;
                break;
            }
            case 'h': {
                for (int i = 0; i < 4 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "head") {
                    return false;
                }

                req->method_ = HttpMethod::head_;
                break;
            }
            case 'c': {
                for (int i = 0; i < 7 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "comment") {
                    return false;
                }

                req->method_ = HttpMethod::comment_;
                break;
            }
            case 't': {
                for (int i = 0; i < 5 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_int8()));
                }

                if (method != "trace") {
                    return false;
                }

                req->method_ = HttpMethod::trace_;
                break;
            }

            default: break;
        }

        if (req->method_ == HttpMethod::invalid_) {
            return false;
        }

        if (buff.read_int8() != ' ') {
            return false;
        }

        return true;
    }

    bool HttpRequestParser::parse_url(Buffer &buff)
    {
        if (header_state != HeaderState::method_gap || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::url;
        std::string url;
        char ch = buff.read_int8();
        url.push_back(ch);
        while (ch != ' ' && buff.readable_bytes()) {
            ch = buff.read_int8();
            if (ch != ' ') {
                url.push_back(ch);
            }
        }

        url = url::url_decode(url);
        req->url_ = url;

        if (!url::decode_url_domain(url, req->url_domain_)) {
            return false;
        }

        if (!url::decode_parameters(url, req->request_params_)) {
            return false;
        }

        return true;
    }

    bool HttpRequestParser::parse_version(Buffer &buff)
    {
        if (header_state != HeaderState::url_gap || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::version;

        std::string version;
        char ch = buff.read_int8();
        version.push_back(ch);
        while (ch != '\r' && buff.readable_bytes()) {
            ch = buff.read_int8();
            if (ch != '\r') {
                version.push_back(ch);
            }
        }

        if (buff.read_int8() != '\n') {
            return false;
        }

        size_t tag_pos = version.find_first_of("/");
        if (tag_pos == std::string::npos) {
            return false;
        }

        std::string tag;
        for (size_t i = 0; i < tag_pos; ++i) {
            tag.push_back(std::tolower(version[i]));
        }

        if (tag.empty() || tag != "http") {
            return false;
        }

        std::string v = version.substr(tag_pos + 1);
        if (v.empty()) {
            return false;
        }

        if (v == "1.0") {
            req->version_ = HttpVersion::v_1_0;
        } else if (v == "1.1") {
            req->version_ = HttpVersion::v_1_1;
        } else if (v == "2.0") {
            req->version_ = HttpVersion::v_2_0;
        } else if (v == "3.0") {
            req->version_ = HttpVersion::v_3_0;
        } else {
            return false;
        }

        return true; 
    }

    bool HttpRequestParser::parse_header_keys(Buffer &buff)
    {
        if (buff.readable_bytes() == 0 || header_state != HeaderState::version_newline) {
            return false;
        }

        while (buff.readable_bytes()) {
            header_state = HeaderState::header_key;
            std::string key;
            char ch = buff.read_int8();
            if (ch == '\r') {
                break;
            }

            while (ch != ':' && buff.readable_bytes()) {
                key.push_back(std::tolower(ch));
                if (ch != ':') {
                    ch = buff.read_int8();
                }
            }

            if (key.empty()) {
                return false;
            }

            header_state = HeaderState::header_value;
            std::string val;
            ch = buff.read_int8();
            if (!std::isblank(ch)) {
                val.push_back(ch);
            }

            while (ch != '\r' && buff.readable_bytes()) {
                ch = buff.read_int8();
                if (ch != '\r') {
                    val.push_back(ch);
                }
            }

            if (buff.read_int8() != '\n') {
                return false;
            }

            req->headers_[key] = val;
        }

        if (buff.read_int8() != '\n') {
            return false;
        }

        return true;
    }

    bool HttpRequestParser::parse_new_line(Buffer &buff)
    {
        char ch = buff.read_int8();
        if (ch != '\r') {
            return false;
        }

        ch = buff.read_int8();
        if (ch != '\n') {
            return false;
        }

        return true;
    }

    uint32_t HttpRequestParser::get_body_length()
    {
        const std::string *length = req->get_header(http_header_key::content_length);
        if (!length) {
            return 0;
        }

        return std::atoi(length->c_str());
    }

    bool HttpRequestParser::parse_header(Buffer &buff)
    {
        int from = buff.get_read_index();
        if (header_state == HttpRequestParser::HeaderState::init) {
            if (!parse_method(buff)) {
                buff.reset_read_index(from);
                header_state = HttpRequestParser::HeaderState::init;
                return false;
            }

            header_state = HeaderState::method_gap;
        }

        from = buff.get_read_index();
        if (header_state == HttpRequestParser::HeaderState::method_gap) {
            if (!parse_url(buff)) {
                buff.reset_read_index(from);
                header_state = HttpRequestParser::HeaderState::method_gap;
                return false;
            }

            header_state = HeaderState::url_gap;
        }

        from = buff.get_read_index();
        if (header_state == HttpRequestParser::HeaderState::url_gap) {
            if (!parse_version(buff)) {
                buff.reset_read_index(from);
                header_state = HttpRequestParser::HeaderState::url_gap;
                return false;
            }

            header_state = HeaderState::version_newline;
        }

        from = buff.get_read_index();
        if (header_state == HttpRequestParser::HeaderState::version_newline) {
            if (!parse_header_keys(buff)) {
                buff.reset_read_index(from);
                header_state = HttpRequestParser::HeaderState::version_newline;
                return false;
            }

            header_state = HeaderState::header_end_lines;
        }
    
        return true;
    }

    int HttpRequestParser::parse_body(Buffer &buff, uint32_t length)
    {
        if (!is_header_done()) {
            return 0; 
        }

        if (length > config::client_max_content_length) {
            return -1;
        }

        if (buff.readable_bytes() < length) {
            return 0;
        }

        return 1;
    }

    int HttpRequestParser::parse(Buffer &buff)
    {
        if (!is_header_done()) {
            parse_header(buff);
        }

        if (is_header_done()) {
            uint32_t length = get_body_length();
            if (length > 0) {
                int res = parse_body(buff, length);
                if (res > 0) {
                    body_state = BodyState::fully;
                    req->body_length_ = length;
                } else if (res == 0) {
                    body_state = BodyState::partial;
                }
                return res;
            } else {
                body_state = BodyState::fully;
                return 1;
            }
        }

        return is_body_done();
    }

    HttpRequest::HttpRequest(HttpRequestContext *context) : context_(context), is_good_(false)
    {
        parser_.set_req(this);
        reset();
    }

    HttpRequest::~HttpRequest()
    {
        if (body_content_) {
            delete body_content_;
            body_content_ = nullptr;
        }
    }

    HttpMethod HttpRequest::get_method() const
    {
        return this->method_;
    }

    std::string HttpRequest::get_raw_method() const
    {
        if (!is_ok()) {
            return {};
        }

        return http_method_descs[(uint32_t)method_];
    }

    HttpVersion HttpRequest::get_version() const
    {
        return this->version_;
    }

    std::string HttpRequest::get_raw_version() const
    {
        if (!is_ok()) {
            return {};
        }

        return http_version_descs[(uint32_t)version_];
    }

    bool HttpRequest::header_exists(const std::string &key) const
    {
        auto it = this->headers_.find(key);
        return it == this->headers_.end();
    }

    const std::string * HttpRequest::get_header(const std::string &key) const
    {
        auto it = this->headers_.find(key);
        return it != this->headers_.end() ? &it->second : nullptr;
    }

    bool HttpRequest::parse(Buffer &buff)
    {
        if (is_ok()) {
            return true;
        }

        //std::string data(buff.peek(), buff.peek() + buff.readable_bytes());
        //std::cout << data << std::endl;

        int res = parser_.parse(buff);
        if (res < 0) {
            is_good_ = false;
            return false;
        } else {
            if (res == 1) {
                const std::string *ctype = get_header(http_header_key::content_type);
                is_good_ = true;
                if (ctype) {
                    is_good_ = parse_content_type(ctype->c_str(), ctype->c_str() + ctype->size(), content_type_text_, content_type_extra_).first;
                    content_type_ = find_content_type(content_type_text_);
                }
                return true;
            } else {
                is_good_ = true;
                return false;
            }
        }
    }

    void HttpRequest::reset()
    {
        is_good_ = false;
        body_length_ = 0;
        url_domain_.clear();
        method_ = HttpMethod::invalid_;
        version_ = HttpVersion::invalid;
        request_params_.clear();
        headers_.clear();
        parser_.reset();
        content_type_ = content_type::not_support;
        error_code_ = ResponseCode::bad_request;
        content_type_ = content_type::not_support;

        if (body_content_) {
            delete body_content_;
            body_content_ = nullptr;
        }

        content_type_extra_.clear();
        error_code_ = ResponseCode::internal_server_error;
    }

    const char * HttpRequest::body_begin()
    {
        return body_length_ == 0 ? nullptr : context_->get_connection()->get_input_buff()->peek();
    }

    const char * HttpRequest::body_end()
    {
        return body_length_ == 0 ? nullptr : context_->get_connection()->get_input_buff()->peek() + body_length_;
    }
    
    void HttpRequest::read_body_done()
    {
        if (body_length_ > 0) {
            context_->get_connection()->get_input_buff()->add_read_index(body_length_);
        }
    }

    std::pair<bool, uint32_t> HttpRequest::parse_content_type(const char *begin, const char *end, std::string &ctype, std::unordered_map<std::string, std::string> &extra)
    {
        const char *p = begin;
        if (!begin) {
            return {true, 0};;
        }

        if (!end || end - begin == 0) {
            return {false, 0};;
        }

        for (; begin != end; ++begin) {
            char ch = *begin;
            if (ch == ' ') continue;

            if (ch == ';') {
                ++begin;
                break;
            }

            if (ch == '\r') {
                begin += 2;
                break;
            }

            ctype.push_back(std::tolower(ch));
        }

        if (begin != end) {
            while (begin != end) {
                char ch = *begin;
                if (ch == ' ') {
                    ++begin;
                    continue;
                }

                if (ch == '\r') {
                    break;
                }

                std::string k;
                for (; begin != end; ++begin) {
                    ch = *begin;
                    if (ch == '=') {
                        ++begin;
                        break;
                    }
                    k.push_back(std::tolower(ch));
                }

                std::string v;
                for (; begin != end; ++begin) {
                    ch = *begin;
                    if (ch == ';' || ch == '\r') {
                        break;
                    }
                    v.push_back(std::tolower(ch));
                }

                extra[k] = v;

                if (*begin == ';') {
                    ++begin;
                    continue;
                }
            }
        }
        return {true, begin - p};
    }

    bool HttpRequest::parse_content()
    {
        if (!is_good_) {
            return false;
        }

        const std::string *ctype = get_header(http_header_key::content_type);
        if (!ctype) {
            return true;
        }

        return singleton::singleton<ContentParserFactory>().parse_content(this);
    }
}
