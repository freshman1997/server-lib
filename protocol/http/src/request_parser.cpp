#include "ops/option.h"
#include "packet_parser.h"
#include "request.h"
#include "request_parser.h"
#include "url.h"

#include <cctype>
#include <cstring>
#include <string>

namespace yuan::net::http
{
    bool HttpRequestParser::parse_method(::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() == 0) {
            return false;
        }

        if (!packet_) {
            return false;
        }
        HttpRequest *req = static_cast<HttpRequest *>(packet_);

        header_state = HeaderState::metohd;
        const char *data = buff.read_ptr();
        const std::size_t readable = buff.readable_bytes();
        if (readable >= 4 &&
            (data[0] == 'G' || data[0] == 'g') &&
            (data[1] == 'E' || data[1] == 'e') &&
            (data[2] == 'T' || data[2] == 't') &&
            data[3] == ' ') {
            req->method_ = HttpMethod::get_;
            buff.consume(4);
            return true;
        }

        char method[16];
        std::size_t method_size = 0;
        bool saw_space = false;
        while (buff.readable_bytes()) {
            char ch = buff.read_i8();
            if (ch == ' ') {
                saw_space = true;
                break;
            }
            if (method_size == sizeof(method)) {
                return false;
            }
            method[method_size++] = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }

        if (method_size == 0 || !saw_space) {
            return false;
        }

        switch (method_size) {
        case 3:
            if (std::memcmp(method, "GET", 3) == 0) req->method_ = HttpMethod::get_;
            else if (std::memcmp(method, "PUT", 3) == 0) req->method_ = HttpMethod::put_;
            else if (std::memcmp(method, "ACL", 3) == 0) req->method_ = HttpMethod::acl_;
            break;
        case 4:
            if (std::memcmp(method, "POST", 4) == 0) req->method_ = HttpMethod::post_;
            else if (std::memcmp(method, "HEAD", 4) == 0) req->method_ = HttpMethod::head_;
            else if (std::memcmp(method, "COPY", 4) == 0) req->method_ = HttpMethod::copy_;
            else if (std::memcmp(method, "MOVE", 4) == 0) req->method_ = HttpMethod::move_;
            else if (std::memcmp(method, "LOCK", 4) == 0) req->method_ = HttpMethod::lock_;
            break;
        case 5:
            if (std::memcmp(method, "TRACE", 5) == 0) req->method_ = HttpMethod::trace_;
            else if (std::memcmp(method, "PATCH", 5) == 0) req->method_ = HttpMethod::patch_;
            else if (std::memcmp(method, "MKCOL", 5) == 0) req->method_ = HttpMethod::mkcol_;
            break;
        case 6:
            if (std::memcmp(method, "DELETE", 6) == 0) req->method_ = HttpMethod::delete_;
            else if (std::memcmp(method, "UNLOCK", 6) == 0) req->method_ = HttpMethod::unlock_;
            else if (std::memcmp(method, "REPORT", 6) == 0) req->method_ = HttpMethod::report_;
            else if (std::memcmp(method, "SEARCH", 6) == 0) req->method_ = HttpMethod::search_;
            break;
        case 7:
            if (std::memcmp(method, "OPTIONS", 7) == 0) req->method_ = HttpMethod::options_;
            else if (std::memcmp(method, "COMMENT", 7) == 0) req->method_ = HttpMethod::comment_;
            break;
        case 8:
            if (std::memcmp(method, "PROPFIND", 8) == 0) req->method_ = HttpMethod::propfind_;
            break;
        case 9:
            if (std::memcmp(method, "PROPPATCH", 9) == 0) req->method_ = HttpMethod::proppatch_;
            break;
        default:
            break;
        }

        return req->method_ != HttpMethod::invalid_;
    }

    bool HttpRequestParser::parse_url(::yuan::buffer::ByteBuffer & buff)
    {
        if (!packet_) {
            return false;
        }
        HttpRequest *req = static_cast<HttpRequest *>(packet_);

        if (header_state != HeaderState::method_gap || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::url;
        const char *data = buff.read_ptr();
        const std::size_t size = buff.readable_bytes();
        const char *space = static_cast<const char *>(std::memchr(data, ' ', size));
        if (!space || space == data) {
            return false;
        }
        const std::size_t token_len = static_cast<std::size_t>(space - data);
        if (buff.read_offset() + token_len > config::max_header_length) {
            header_state = HeaderState::too_long;
            return false;
        }
        const bool needs_decode = std::memchr(data, '%', token_len) != nullptr;

        const std::string_view raw_url(data, token_len);
        const std::size_t query_pos = raw_url.find('?');
        req->url_ = needs_decode
                        ? url::url_decode(raw_url.data(), raw_url.data() + raw_url.size())
                        : std::string(raw_url);

        buff.consume(token_len + 1);

        if (req->url_.empty() || (req->url_.front() != '/' && req->url_.find('/') == std::string::npos)) {
            return false;
        }
        req->url_domain_.clear();
        req->url_domain_parsed_ = false;
        req->url_domain_valid_ = true;

        if (query_pos != std::string::npos) {
            if (!url::decode_parameters(raw_url.substr(query_pos), req->params_)) {
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
