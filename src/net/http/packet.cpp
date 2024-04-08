#include "net/http/packet.h"
#include "buffer/pool.h"
#include "net/base/connection/connection.h"
#include "net/http/content/content_parser_factory.h"
#include "net/http/context.h"
#include "net/http/header_key.h"
#include "net/http/packet_parser.h"
#include "singleton/singleton.h"

namespace net::http 
{
    static const char* http_version_descs[4] = {
        "1.0",
        "1.1",
        "2.0",
        "3.0"
    };

    HttpPacket::HttpPacket(HttpSessionContext *context) : context_(context)
    {
        body_content_ = nullptr;
        parser_ = nullptr;
        buffer_ = nullptr;
        body_length_ = 0;
        buffer_ = singleton::Singleton<BufferedPool>().allocate();
    }

    HttpPacket::~HttpPacket()
    {
        if (body_content_) {
            delete body_content_;
            body_content_ = nullptr;
        }

        if (parser_) {
            delete parser_;
            parser_ = nullptr;
        }

        singleton::Singleton<BufferedPool>().free(buffer_);
    }

    void HttpPacket::reset()
    {
        is_good_ = false;
        body_length_ = 0;
        version_ = HttpVersion::v_1_1;
        params_.clear();
        headers_.clear();
        content_type_text_.clear();
        parser_->reset();
        content_type_ = ContentType::not_support;
        error_code_ = ResponseCode::bad_request;
        content_type_ = ContentType::not_support;

        if (body_content_) {
            delete body_content_;
        }

        body_content_ = nullptr;

        content_type_extra_.clear();
        error_code_ = ResponseCode::internal_server_error;
        buffer_->reset();
    }

    const std::string * HttpPacket::get_header(const std::string &key)
    {
        auto it = headers_.find(key);
        return it != headers_.end() ? &it->second : nullptr;
    }

    void HttpPacket::add_header(const std::string &k, const std::string &v)
    {
        headers_[k] = v;
    }

    void HttpPacket::set_body_length(uint32_t len)
    {
        body_length_ = len;
    }

    bool HttpPacket::is_ok() const 
    {
        return parser_->done();
    }

    void HttpPacket::set_version(HttpVersion ver)
    {
        version_ = ver;
    }

    HttpVersion HttpPacket::get_version() const
    {
        return this->version_;
    }

    std::string HttpPacket::get_raw_version() const
    {
        if (!is_ok()) {
            return {};
        }

        return http_version_descs[(uint32_t)version_];
    }

    const char * HttpPacket::body_begin()
    {
        return body_length_ == 0 ? nullptr : buffer_->readable_bytes() > 0 
            ? buffer_->peek() 
            : context_->get_connection()->get_input_buff()->peek();
    }

    const char * HttpPacket::body_end()
    {
        return body_length_ == 0 ? nullptr : 
            buffer_->readable_bytes() > 0 
            ? buffer_->peek() + body_length_ 
            : context_->get_connection()->get_input_buff()->peek() + body_length_;
    }
    
    void HttpPacket::read_body_done()
    {
        if (body_length_ > 0) {
            context_->get_connection()->get_input_buff()->add_read_index(body_length_);
        }
    }

    std::pair<bool, uint32_t> HttpPacket::parse_content_type(const char *begin, const char *end, std::string &ctype, std::unordered_map<std::string, std::string> &extra)
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

    bool HttpPacket::parse_content()
    {
        if (!is_good_) {
            return false;
        }

        const std::string *ctype = get_header(http_header_key::content_type);
        if (!ctype) {
            return true;
        }

        return singleton::Singleton<ContentParserFactory>().parse_content(this);
    }

    bool HttpPacket::parse(Buffer &buff)
    {
        if (is_ok()) {
            return true;
        }

        //std::string data(buff.peek(), buff.peek() + buff.readable_bytes());
        //std::cout << data << std::endl;

        int res = parser_->parse(buff);
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

    void HttpPacket::send()
    {
        bool res = pack_header();
        if (res) {
            if (buffer_) {
                context_->get_connection()->write(buffer_);
                buffer_ = singleton::Singleton<BufferedPool>().allocate();
            }
        } else {
            is_good_ = false;
        }
    }

    Buffer * HttpPacket::get_buff(bool take)
    {
        if (!take) {
            return buffer_;
        }

        Buffer *buf = buffer_;
        buffer_ = singleton::Singleton<BufferedPool>().allocate();
        buf->reset_read_index(0);
        
        return buf;
    }
}