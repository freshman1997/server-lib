#include "ops/option.h"
#include "packet_parser.h"
#include "request.h"
#include "request_parser.h"
#include "url.h"

namespace yuan::net::http 
{
    bool HttpRequestParser::parse_method(::yuan::buffer::ByteBuffer &buff)
    {
        if (buff.readable_bytes() == 0) {
            return false;
        }

        HttpRequest * req = dynamic_cast<HttpRequest *>(packet_);
        if (!req) {
            return false;
        }

        header_state = HeaderState::metohd;
        char ch = std::tolower(buff.read_i8());
        std::string method;
        method.push_back(ch);
        switch (ch) {
            case 'g': {
                for (int i = 0; i < 2 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method != "get") {
                    return false;
                }

                req->method_ = HttpMethod::get_;
                break;
            }
            case 'p': {
                for (int i = 0; i < 2 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method == "put") {
                    req->method_ = HttpMethod::put_;
                    break;
                }

                method.push_back(std::tolower(buff.read_i8()));
                if (method == "post") {
                    req->method_ = HttpMethod::post_;
                    break;
                }

                method.push_back(std::tolower(buff.read_i8()));
                if (method != "patch") {
                    return false;
                }

                req->method_ = HttpMethod::patch_;
                break;
            }
            case 'd': {
                for (int i = 0; i < 6 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method != "delete") {
                    return false;
                }

                req->method_ = HttpMethod::delete_;
                break;
            }
            case 'o': {
                for (int i = 0; i < 6 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method != "options") {
                    return false;
                }

                req->method_ = HttpMethod::options_;
                break;
            }
            case 'h': {
                for (int i = 0; i < 4 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method != "head") {
                    return false;
                }

                req->method_ = HttpMethod::head_;
                break;
            }
            case 'c': {
                for (int i = 0; i < 7 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
                }

                if (method != "comment") {
                    return false;
                }

                req->method_ = HttpMethod::comment_;
                break;
            }
            case 't': {
                for (int i = 0; i < 5 && buff.readable_bytes(); ++i) {
                    method.push_back(std::tolower(buff.read_i8()));
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

        if (buff.read_i8() != ' ') {
            return false;
        }

        return true;
    }

    bool HttpRequestParser::parse_url(::yuan::buffer::ByteBuffer &buff)
    {
        HttpRequest * req = dynamic_cast<HttpRequest *>(packet_);
        if (!req) {
            return false;
        }

        if (header_state != HeaderState::method_gap || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::url;
        std::string url;
        char ch = buff.read_i8();
        url.push_back(ch);
        while (ch != ' ' && buff.readable_bytes()) {
            ch = buff.read_i8();
            if (ch != ' ') {
                url.push_back(ch);
            }

            if (buff.read_offset() > config::client_max_content_length) {
                header_state = HeaderState::too_long;
                return false;
            }
        }

        url = url::url_decode(url);

        if (!url::decode_url_domain(url, req->url_domain_)) {
            return false;
        }

        if (!url::decode_parameters(url, req->params_)) {
            return false;
        }

        if (const size_t pos = url.find_first_of('?'); pos != std::string::npos) {
            req->url_ = url.substr(0, pos);
        } else {
            req->url_ = url;
        }

        return true;
    }

    bool HttpRequestParser::parse_header(::yuan::buffer::ByteBuffer &buff)
    {
        auto from = buff.read_offset();
        if (header_state == HeaderState::init) {
            if (!parse_method(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::init;
                return false;
            }

            header_state = HeaderState::method_gap;
        }

        if (buff.read_offset() > config::max_header_length) {
            header_state = HeaderState::too_long;
            return false;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::method_gap) {
            if (!parse_url(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::method_gap;
                return false;
            }

            header_state = HeaderState::url_gap;
        }

        if (buff.read_offset() > config::max_header_length) {
            header_state = HeaderState::too_long;
            return false;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::url_gap) {
            if (!parse_version(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::url_gap;
                return false;
            }

            header_state = HeaderState::version_newline;
        }

        if (buff.read_offset() > config::max_header_length) {
            header_state = HeaderState::too_long;
            return false;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::version_newline) {
            if (!parse_header_keys(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::version_newline;
                packet_->clear_header();
                return false;
            }

            header_state = HeaderState::header_end_lines;
        }

        if (buff.read_offset() > config::max_header_length) {
            header_state = HeaderState::too_long;
            return false;
        }
    
        return true;
    }
}
