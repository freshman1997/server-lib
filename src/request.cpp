#include "request.h"
#include "url.h"
#include <cctype>
#include <string>

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

            req->method = HttpMethod::get_;
            break;
        }
        case 'p': {
            for (int i = 0; i < 2 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method == "put") {
                req->method = HttpMethod::put_;
                break;
            }

            method.push_back(std::tolower(buff.read_int8()));
            if (method != "post") {
                return false;
            }

            req->method = HttpMethod::post_;
            break;
        }
        case 'd': {
            for (int i = 0; i < 6 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method != "delete") {
                return false;
            }

            req->method = HttpMethod::delete_;
            break;
        }
        case 'o': {
            for (int i = 0; i < 7 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method != "options") {
                return false;
            }

            req->method = HttpMethod::options_;
            break;
        }
        case 'h': {
            for (int i = 0; i < 4 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method != "head") {
                return false;
            }

            req->method = HttpMethod::head_;
            break;
        }
        case 'c': {
            for (int i = 0; i < 7 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method != "comment") {
                return false;
            }

            req->method = HttpMethod::comment_;
            break;
        }
        case 't': {
            for (int i = 0; i < 5 && buff.readable_bytes(); ++i) {
                method.push_back(std::tolower(buff.read_int8()));
            }

            if (method != "trace") {
                return false;
            }

            req->method = HttpMethod::trace_;
            break;
        }

        default: break;
    }

    if (req->method == HttpMethod::invalid_) {
        return false;
    }

    if (buff.read_int8() != ' ') {
        return false;
    }

    header_state = HeaderState::method_gap;
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

    if (url.size() == 1) {
        header_state = HeaderState::method_gap;
        return false;
    }

    url = url::url_decode(url);

    if (!url::decode_url_domain(url, req->url_domain)) {
        return false;
    }

    if (!url::decode_parameters(url, req->request_params)) {
        return false;
    }

    header_state = HeaderState::url_gap;
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
        req->version = HttpVersion::v_1_0;
    } else if (v == "1.1") {
        req->version = HttpVersion::v_1_0;
    } else if (v == "2.0") {
        req->version = HttpVersion::v_2_0;
    } else if (v == "3.0") {
        req->version = HttpVersion::v_3_0;
    } else {
        return false;
    }

    header_state = HeaderState::version_newline;
    return true; 
}

bool HttpRequestParser::parse_headers(Buffer &buff)
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
            key.push_back(ch);
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

        if (val.empty() || buff.read_int8() != '\n') {
            return false;
        }

        req->headers[key] = val;
    }

    if (buff.read_int8() != '\n') {
        return false;
    }

    header_state = HeaderState::header_newline;
    return true;
}

bool HttpRequestParser::parse_body(Buffer &buff)
{
    return false;
}

HttpRequest::HttpRequest() 
{
    parser.set_req(this);
}

HttpMethod HttpRequest::get_method() const
{
    return this->method;
}

HttpVersion HttpRequest::get_version() const
{
    return this->version;
}

bool HttpRequest::header_exists(std::string &key) const
{
    auto it = this->headers.find(key);
    return it == this->headers.end();
}

bool HttpRequest::parse_header(Buffer &buff)
{
    if (!parser.parse_method(buff) || !parser.parse_url(buff) 
            || !parser.parse_version(buff) || !parser.parse_headers(buff)) {
        return false;
    }

    return true;
}

