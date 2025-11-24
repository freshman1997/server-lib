#include "packet.h"
#include "buffer/pool.h"
#include "net/connection/connection.h"
#include "content/content_parser_factory.h"
#include "context.h"
#include "header_key.h"
#include "packet_parser.h"
#include "content/content_parser.h"

namespace yuan::net::http 
{
    static const char* http_version_descs[4] = {
        "1.0",
        "1.1",
        "2.0",
        "3.0"
    };

    HttpPacket::HttpPacket(HttpSessionContext *context) : context_(context), error_code_(ResponseCode::forbidden), content_type_(ContentType::not_support)
    {
        body_content_ = nullptr;
        parser_ = nullptr;
        body_length_ = 0;
        pre_content_parser_ = nullptr;
        task_ = nullptr;
    }

    HttpPacket::~HttpPacket()
    {
        HttpPacket::reset();
        if (body_content_) {
            delete body_content_;
            body_content_ = nullptr;
        }

        if (parser_) {
            delete parser_;
            parser_ = nullptr;
        }
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

        delete body_content_;
        body_content_ = nullptr;

        content_type_extra_.clear();
        error_code_ = ResponseCode::internal_server_error;

        delete pre_content_parser_;

        pre_content_parser_ = nullptr;
        chunked_checksum_.clear();

        is_download_file_ = false;
        is_upload_file_ = false;
        if (task_) {
            delete task_;
            task_ = nullptr;
        }

        original_file_name_.clear();
        reader_.init();
    }

    const std::string * HttpPacket::get_header(const std::string &key)
    {
        const auto it = headers_.find(key);
        return it != headers_.end() ? &it->second : nullptr;
    }

    void HttpPacket::add_header(const std::string &k, const std::string &v)
    {
        headers_[k] = v;
    }

    void HttpPacket::remove_header(const std::string &k)
    {
        headers_.erase(k);
    }

    void HttpPacket::clear_header()
    {
        headers_.clear();
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
                }
            }
        }
        return {true, begin - p};
    }

    std::pair<bool, uint32_t> HttpPacket::parse_content_type(buffer::BufferReader &reader, std::string &ctype, std::unordered_map<std::string, std::string> &extra)
    {
        if (reader.readable_bytes() == 0) {
            return {true, 0};;
        }

        const size_t from = reader.get_read_offset();
        for (; reader; ++reader) {
            char ch = *reader;
            if (ch == ' ') continue;

            if (ch == ';') {
                break;
            }

            if (ch == '\r' || ch == '\n') {
                if (!reader.skip_newline_symbol()) {
                    return {false, 0};
                }
                return {true, reader.get_read_offset() - from};
            }

            ctype.push_back(std::tolower(ch));
        }

        while (reader) {
            char ch = *reader;
            if (ch == ' ') {
                ++reader;
                continue;
            }

            if (ch == '\r') {
                break;
            }

            std::string k;
            for (; reader; ++reader) {
                ch = *reader;
                if (ch == '=') {
                    ++reader;
                    break;
                }
                k.push_back(std::tolower(ch));
            }

            std::string v;
            for (; reader; ++reader) {
                ch = *reader;
                if (ch == ';' || ch == '\r') {
                    break;
                }
                v.push_back(std::tolower(ch));
            }

            extra[k] = v;

            if (*reader == ';') {
                ++reader;
            }
        }
        return {true, reader.get_read_offset() - from};
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

        is_good_ = ContentParserFactory::get_instance()->parse_content(this);

        return is_good_;
    }

    bool HttpPacket::parse(const std::vector<buffer::Buffer *> &buffers)
    {
        reader_.add_buffer(buffers);
        if (reader_.readable_bytes() == 0) {
            return false;
        }

        if (is_ok() && !is_downloading()) {
            return true;
        }

        if (is_downloading()) {
            if (!task_) {
                return true;
            }

            task_->on_data(reader_);
            if (!task_->is_good()) {
                is_good_ = false;
                return false;
            }

            return true;
        }

        const int res = parser_->parse(reader_);
        if (res < 0) {
            is_good_ = false;
            return false;
        }

        if (res == 1) {
            const std::string *ctype = get_header(http_header_key::content_type);
            is_good_ = true;
            if (ctype) {
                is_good_ = parse_content_type(ctype->c_str(), ctype->c_str() + ctype->size(), content_type_text_, content_type_extra_).first;
                content_type_ = find_content_type(content_type_text_);
            }
            return true;
        }

        is_good_ = true;
        return false;
    }

    bool HttpPacket::write(buffer::BufferReader &reader) const
    {
        if (is_uploading()) {
            if (task_) {
                return task_->on_data(reader);
            }
        }

        return false;
    }

    void HttpPacket::send()
    {
        pack_and_send(context_->get_connection());
    }

    void HttpPacket::allocate_body(size_t sz)
    {
        reader_.add_buffer(buffer::BufferedPool::get_instance()->allocate(sz));
    }

    void HttpPacket::pack_and_send(Connection *conn)
    {
        assert(conn);
        if (pack_header(conn)) {
            if (reader_.readable_bytes() > 0) {
                for (const auto &buffers = reader_.take_buffers(); const auto &buffer : buffers) {
                    conn->write(buffer);
                }
            }
        } else {
            is_good_ = false;
        }
        conn->flush();
    }

    bool HttpPacket::is_chunked() const
    {
        auto it = headers_.find(http_header_key::transfer_encoding);
        if (it != headers_.end()) {
            return it->second == "chunked";
        }

        it = headers_.find(http_header_key::accept_encoding);
        if (it != headers_.end()) {
            return it->second == "chunked";
        }

        return false;
    }

    std::string HttpPacket::get_content_charset() const
    {
        auto it = content_type_extra_.find("charset");
        if (it != content_type_extra_.end()) {
            return it->second;
        }
        return {};
    }

    void HttpPacket::set_body_state(BodyState state)
    {
        parser_->set_body_state(state);
    }
}