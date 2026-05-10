#include "ops/option.h"
#include "packet_parser.h"
#include "request.h"
#include "request_parser.h"
#include "url.h"

#include <cctype>
#include <string>

namespace yuan::net::http
{
    bool HttpRequestParser::parse_method(::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() == 0) {
            return false;
        }

        HttpRequest *req = dynamic_cast<HttpRequest *>(packet_);
        if (!req) {
            return false;
        }

        header_state = HeaderState::metohd;
        std::string method;
        bool saw_space = false;
        while (buff.readable_bytes()) {
            char ch = buff.read_i8();
            if (ch == ' ') {
                saw_space = true;
                break;
            }
            method.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            if (method.size() > 16) {
                return false;
            }
        }

        if (method.empty() || !saw_space) {
            return false;
        }

        if (method == "GET") req->method_ = HttpMethod::get_;
        else if (method == "POST") req->method_ = HttpMethod::post_;
        else if (method == "PUT") req->method_ = HttpMethod::put_;
        else if (method == "DELETE") req->method_ = HttpMethod::delete_;
        else if (method == "OPTIONS") req->method_ = HttpMethod::options_;
        else if (method == "HEAD") req->method_ = HttpMethod::head_;
        else if (method == "COMMENT") req->method_ = HttpMethod::comment_;
        else if (method == "TRACE") req->method_ = HttpMethod::trace_;
        else if (method == "PATCH") req->method_ = HttpMethod::patch_;
        else if (method == "PROPFIND") req->method_ = HttpMethod::propfind_;
        else if (method == "PROPPATCH") req->method_ = HttpMethod::proppatch_;
        else if (method == "MKCOL") req->method_ = HttpMethod::mkcol_;
        else if (method == "COPY") req->method_ = HttpMethod::copy_;
        else if (method == "MOVE") req->method_ = HttpMethod::move_;
        else if (method == "LOCK") req->method_ = HttpMethod::lock_;
        else if (method == "UNLOCK") req->method_ = HttpMethod::unlock_;
        else if (method == "REPORT") req->method_ = HttpMethod::report_;
        else if (method == "ACL") req->method_ = HttpMethod::acl_;
        else if (method == "SEARCH") req->method_ = HttpMethod::search_;

        return req->method_ != HttpMethod::invalid_;
    }

    bool HttpRequestParser::parse_url(::yuan::buffer::ByteBuffer & buff)
    {
        HttpRequest *req = dynamic_cast<HttpRequest *>(packet_);
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

            if (buff.read_offset() > config::max_header_length) {
                header_state = HeaderState::too_long;
                return false;
            }
        }

        size_t query_pos = url.find_first_of('?');
        req->url_ = url::url_decode(url);

        if (!url::decode_url_domain(req->url_, req->url_domain_)) {
            return false;
        }

        if (query_pos != std::string::npos) {
            std::string query_part = url.substr(query_pos);
            if (!url::decode_parameters(query_part, req->params_)) {
                return false;
            }
        }

        return true;
    }

    bool HttpRequestParser::parse_header(::yuan::buffer::ByteBuffer & buff)
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
