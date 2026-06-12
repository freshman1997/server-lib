#include "packet.h"
#include "content/content_parser.h"
#include "content/content_parser_factory.h"
#include "context.h"
#include "header_key.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "ops/option.h"
#include "packet_parser.h"
#include "logger.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <string_view>

namespace yuan::net::http
{
    namespace
    {
        std::atomic_uint64_t g_body_spool_counter{0};

        std::filesystem::path make_body_spool_path()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto id = g_body_spool_counter.fetch_add(1, std::memory_order_relaxed);
            return std::filesystem::temp_directory_path() /
                   ("yuan_http_body_" + std::to_string(now) + "_" + std::to_string(id) + ".tmp");
        }

        bool has_complete_http_header(const ::yuan::buffer::ByteBuffer &buffer)
        {
            const auto size = buffer.readable_bytes();
            if (size < 4) {
                return false;
            }

            const char *data = buffer.read_ptr();
            for (std::size_t i = 0; i + 3 < size; ++i) {
                if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
                    return true;
                }
            }
            return false;
        }

        bool token_equals_ci(std::string_view token, std::string_view expected)
        {
            if (token.size() != expected.size()) {
                return false;
            }
            for (std::size_t i = 0; i < token.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(token[i])) != expected[i]) {
                    return false;
                }
            }
            return true;
        }

        bool transfer_encoding_has_chunked(std::string_view value)
        {
            std::size_t pos = 0;
            while (pos < value.size()) {
                const auto comma = value.find(',', pos);
                auto end = comma == std::string_view::npos ? value.size() : comma;
                while (pos < end && (value[pos] == ' ' || value[pos] == '\t')) {
                    ++pos;
                }
                while (end > pos && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
                    --end;
                }
                if (token_equals_ci(value.substr(pos, end - pos), "chunked")) {
                    return true;
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                pos = comma + 1;
            }
            return false;
        }

        int parsed_header_slot(std::string_view key) noexcept
        {
            const char *data = key.data();
            if (data == http_header_key::host) return 0;
            if (data == http_header_key::connection) return 1;
            if (data == http_header_key::content_length) return 2;
            if (data == http_header_key::content_type) return 3;
            if (data == http_header_key::transfer_encoding) return 4;
            if (data == http_header_key::accept) return 5;
            if (data == http_header_key::accept_encoding) return 6;
            if (data == http_header_key::user_agent) return 7;

            if (header_key_equals_ci(key, http_header_key::host)) return 0;
            if (header_key_equals_ci(key, http_header_key::connection)) return 1;
            if (header_key_equals_ci(key, http_header_key::content_length)) return 2;
            if (header_key_equals_ci(key, http_header_key::content_type)) return 3;
            if (header_key_equals_ci(key, http_header_key::transfer_encoding)) return 4;
            if (header_key_equals_ci(key, http_header_key::accept)) return 5;
            if (header_key_equals_ci(key, http_header_key::accept_encoding)) return 6;
            if (header_key_equals_ci(key, http_header_key::user_agent)) return 7;
            return -1;
        }

        const char *parsed_header_name(std::size_t slot) noexcept
        {
            switch (slot) {
            case 0: return http_header_key::host;
            case 1: return http_header_key::connection;
            case 2: return http_header_key::content_length;
            case 3: return http_header_key::content_type;
            case 4: return http_header_key::transfer_encoding;
            case 5: return http_header_key::accept;
            case 6: return http_header_key::accept_encoding;
            case 7: return http_header_key::user_agent;
            default: return "";
            }
        }
    }

    static const char *http_version_descs[4] = {
        "1.0",
        "1.1",
        "2.0",
        "3.0"
    };

    HttpPacket::HttpPacket(HttpSessionContext * context)
        : context_(context)
    {
        body_content_ = nullptr;
        headers_.reserve(8);
        params_.reserve(4);
        content_type_extra_.reserve(2);

        body_length_ = 0;
        is_good_ = false;
        error_code_ = ResponseCode::internal_server_error;

    }

    HttpPacket::~HttpPacket()
    {
        reset();
    }

    HttpPacket::HttpPacket(HttpPacket && other) noexcept
        : version_(other.version_),
          is_good_(other.is_good_),
          is_upload_file_(other.is_upload_file_),
          is_download_file_(other.is_download_file_),
          error_code_(other.error_code_),
          content_type_(other.content_type_),
          body_length_(other.body_length_),
          body_related_header_present_(other.body_related_header_present_),
          context_(other.context_),
          parser_(std::move(other.parser_)),
          params_(std::move(other.params_)),
          headers_(std::move(other.headers_)),
          content_type_text_(std::move(other.content_type_text_)),
          content_type_extra_(std::move(other.content_type_extra_)),
          body_content_(std::move(other.body_content_)),
          buffer_(std::move(other.buffer_)),
          input_cache_(std::move(other.input_cache_)),
          pre_content_parser_(std::move(other.pre_content_parser_)),
          task_(std::move(other.task_)),
          chunked_checksum_(std::move(other.chunked_checksum_)),
          original_file_name_(std::move(other.original_file_name_)),
          body_file_path_(std::move(other.body_file_path_)),
          body_file_stream_(std::move(other.body_file_stream_)),
          body_file_expected_(other.body_file_expected_),
          body_file_received_(other.body_file_received_),
          body_file_owned_(other.body_file_owned_)
    {
        other.context_ = nullptr;
        other.parser_.reset();
        other.pre_content_parser_.reset();
        other.task_.reset();
        other.body_length_ = 0;
        other.is_good_ = false;
    }

    HttpPacket &HttpPacket::operator=(HttpPacket && other) noexcept
    {
        if (this != &other) {
            reset();

            version_ = other.version_;
            is_good_ = other.is_good_;
            is_upload_file_ = other.is_upload_file_;
            is_download_file_ = other.is_download_file_;
            error_code_ = other.error_code_;
            content_type_ = other.content_type_;
            body_length_ = other.body_length_;
            body_related_header_present_ = other.body_related_header_present_;
            context_ = other.context_;
            parser_ = std::move(other.parser_);
            params_ = std::move(other.params_);
            headers_ = std::move(other.headers_);
            content_type_text_ = std::move(other.content_type_text_);
            content_type_extra_ = std::move(other.content_type_extra_);
            body_content_ = std::move(other.body_content_);
            buffer_ = std::move(other.buffer_);
            input_cache_ = std::move(other.input_cache_);
            pre_content_parser_ = std::move(other.pre_content_parser_);
            task_ = std::move(other.task_);
            chunked_checksum_ = std::move(other.chunked_checksum_);
            original_file_name_ = std::move(other.original_file_name_);
            body_file_path_ = std::move(other.body_file_path_);
            body_file_stream_ = std::move(other.body_file_stream_);
            body_file_expected_ = other.body_file_expected_;
            body_file_received_ = other.body_file_received_;
            body_file_owned_ = other.body_file_owned_;

            other.context_ = nullptr;
            other.parser_.reset();
            other.pre_content_parser_.reset();
            other.task_.reset();
            other.body_length_ = 0;
            other.is_good_ = false;
        }
        return *this;
    }

    void HttpPacket::reset()
    {
        is_good_ = false;
        body_length_ = 0;
        body_related_header_present_ = false;
        version_ = HttpVersion::v_1_1;
        params_.clear();
        headers_.clear();
        clear_parsed_headers();
        content_type_text_.clear();
        if (parser_) {
            parser_->reset();
        }
        content_type_ = ContentType::not_support;
        body_content_.reset();
        content_type_extra_.clear();
        error_code_ = ResponseCode::internal_server_error;
        buffer_.clear();
        input_cache_.clear();
        pre_content_parser_.reset();
        chunked_checksum_.clear();
        is_download_file_ = false;
        is_upload_file_ = false;
        task_.reset();
        original_file_name_.clear();
        if (body_file_stream_) {
            body_file_stream_->close();
            body_file_stream_.reset();
        }
        if (body_file_owned_ && !body_file_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(body_file_path_, ec);
        }
        body_file_path_.clear();
        body_file_expected_ = 0;
        body_file_received_ = 0;
        body_file_owned_ = false;
    }

    void HttpPacket::set_pre_content_parser(ContentParser * parser)
    {
        pre_content_parser_.reset(parser);
    }

    void HttpPacket::set_task(HttpTask * task)
    {
        task_.reset(task);
    }

    const std::string *HttpPacket::get_header(std::string_view key) const
    {
        if (key.empty()) {
            return nullptr;
        }

        const int slot = parsed_header_slot(key);
        if (slot >= 0 && parsed_header_present_.test(static_cast<std::size_t>(slot))) {
            return &parsed_header_values_[static_cast<std::size_t>(slot)];
        }

        const auto it = headers_.find(key);
        if (it != headers_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void HttpPacket::add_header(const std::string & k, const std::string & v)
    {
        remove_header(k);
        headers_[k] = v;
        body_related_header_present_ = body_related_header_present_ ||
            header_key_equals_ci(k, http_header_key::content_length) ||
            header_key_equals_ci(k, http_header_key::content_type) ||
            header_key_equals_ci(k, http_header_key::transfer_encoding);
    }

    void HttpPacket::add_header(std::string && k, std::string && v)
    {
        const bool body_related =
            header_key_equals_ci(k, http_header_key::content_length) ||
            header_key_equals_ci(k, http_header_key::content_type) ||
            header_key_equals_ci(k, http_header_key::transfer_encoding);
        remove_header(k);
        headers_[std::move(k)] = std::move(v);
        body_related_header_present_ = body_related_header_present_ || body_related;
    }

    void HttpPacket::add_header(const char * k, const char * v)
    {
        if (!k || !v) {
            return;
        }
        remove_header(k);
        headers_[k] = v;
        body_related_header_present_ = body_related_header_present_ ||
            header_key_equals_ci(k, http_header_key::content_length) ||
            header_key_equals_ci(k, http_header_key::content_type) ||
            header_key_equals_ci(k, http_header_key::transfer_encoding);
    }

    void HttpPacket::add_header(const char * k, std::string && v)
    {
        if (!k) {
            return;
        }
        remove_header(k);
        headers_[k] = std::move(v);
        body_related_header_present_ = body_related_header_present_ ||
            header_key_equals_ci(k, http_header_key::content_length) ||
            header_key_equals_ci(k, http_header_key::content_type) ||
            header_key_equals_ci(k, http_header_key::transfer_encoding);
    }

    void HttpPacket::add_parsed_header(const char *k, std::string &&v)
    {
        if (!k) {
            return;
        }
        const int slot = parsed_header_slot(k);
        if (slot >= 0) {
            const auto idx = static_cast<std::size_t>(slot);
            parsed_header_values_[idx] = std::move(v);
            parsed_header_present_.set(idx);
            if (k == http_header_key::content_length ||
                k == http_header_key::content_type ||
                k == http_header_key::transfer_encoding) {
                body_related_header_present_ = true;
            }
            return;
        }
        headers_[k] = std::move(v);
    }

    void HttpPacket::remove_header(std::string_view k)
    {
        if (k.empty()) {
            return;
        }
        const int slot = parsed_header_slot(k);
        if (slot >= 0) {
            const auto idx = static_cast<std::size_t>(slot);
            parsed_header_values_[idx].clear();
            parsed_header_present_.reset(idx);
        }
        const auto it = headers_.find(k);
        if (it != headers_.end()) {
            headers_.erase(it);
        }
    }

    void HttpPacket::clear_header()
    {
        headers_.clear();
        HttpHeaderMap empty;
        headers_.swap(empty);
        clear_parsed_headers();
        body_related_header_present_ = false;
    }

    void HttpPacket::clear_parsed_headers()
    {
        for (std::size_t i = 0; i < parsed_header_values_.size(); ++i) {
            if (parsed_header_present_.test(i)) {
                parsed_header_values_[i].clear();
            }
        }
        parsed_header_present_.reset();
    }

    void HttpPacket::materialize_parsed_headers() const
    {
        for (std::size_t i = 0; i < parsed_header_values_.size(); ++i) {
            if (!parsed_header_present_.test(i)) {
                continue;
            }
            headers_[parsed_header_name(i)] = parsed_header_values_[i];
        }
    }

    void HttpPacket::set_body_length(uint32_t len)
    {
        body_length_ = len;
    }

    bool HttpPacket::is_ok() const
    {
        return parser_ ? parser_->done() : false;
    }

    void HttpPacket::set_version(HttpVersion ver)
    {
        version_ = ver;
    }

    HttpVersion HttpPacket::get_version() const
    {
        return version_;
    }

    std::string HttpPacket::get_raw_version() const
    {
        if (!is_ok())
            return {};

        int idx = static_cast<int>(version_);
        if (idx < 0 || idx > 3)
            return {};
        return http_version_descs[idx];
    }

    const char *HttpPacket::body_begin()
    {
        if (body_length_ == 0)
            return nullptr;

        return input_cache_.readable_bytes() > 0 ? input_cache_.read_ptr() : nullptr;
    }

    const char *HttpPacket::body_end()
    {
        if (body_length_ == 0)
            return nullptr;

        return input_cache_.readable_bytes() >= body_length_ ? input_cache_.read_ptr() + body_length_ : nullptr;
    }

    yuan::buffer::ByteBuffer HttpPacket::take_body_buffer()
    {
        auto body = input_cache_.copy_readable();
        input_cache_.clear();
        return body;
    }

    yuan::buffer::ByteBuffer HttpPacket::take_leftover_buffer()
    {
        if (has_body_file() && body_file_spool_done()) {
            auto leftover = input_cache_.copy_readable();
            input_cache_.clear();
            return leftover;
        }

        if (body_length_ >= input_cache_.readable_bytes()) {
            input_cache_.clear();
            return {};
        }

        size_t leftover_size = input_cache_.readable_bytes() - body_length_;
        yuan::buffer::ByteBuffer leftover(leftover_size);
        leftover.append(input_cache_.read_ptr() + body_length_, leftover_size);
        input_cache_.clear();
        return leftover;
    }

    void HttpPacket::replace_body_buffer(yuan::buffer::ByteBuffer buffer)
    {
        input_cache_ = std::move(buffer);
        input_cache_.compact();
    }

    std::pair<bool, uint32_t> HttpPacket::parse_content_type(const char * begin, const char * end, std::string & ctype, std::unordered_map<std::string, std::string> & extra)
    {
        const char *const p = begin;
        if (!begin) {
            return { true, 0 };
        }

        if (!end || end - begin == 0) {
            return { false, 0 };
        }

        const auto consume_crlf_if_any = [&](const char *cur) {
            if (cur < end && *cur == '\r') {
                ++cur;
                if (cur < end && *cur == '\n') {
                    ++cur;
                }
            }
            return cur;
        };

        const auto append_lower_trimmed = [](std::string &out, const char *b, const char *e) {
            while (b < e && std::isspace(static_cast<unsigned char>(*b))) {
                ++b;
            }
            while (e > b && std::isspace(static_cast<unsigned char>(*(e - 1)))) {
                --e;
            }
            out.reserve(out.size() + static_cast<std::size_t>(e - b));
            for (const char *it = b; it < e; ++it) {
                out.push_back(ascii_lower(*it));
            }
        };

        const char *cur = begin;
        while (cur < end && *cur != ';' && *cur != '\r') {
            ++cur;
        }
        append_lower_trimmed(ctype, begin, cur);

        if (cur < end && *cur == ';') {
            ++cur;
        } else {
            cur = consume_crlf_if_any(cur);
            return { true, static_cast<uint32_t>(cur - p) };
        }

        if (ctype.size() < 256) {
            extra.reserve(4);
            while (cur < end) {
                while (cur < end && std::isspace(static_cast<unsigned char>(*cur))) {
                    ++cur;
                }
                if (cur >= end || *cur == '\r') {
                    break;
                }

                const char *key_begin = cur;
                while (cur < end && *cur != '=' && *cur != ';' && *cur != '\r') {
                    ++cur;
                }

                if (cur >= end || *cur != '=') {
                    break;
                }

                const char *key_end = cur;
                ++cur;
                const char *value_begin = cur;
                while (cur < end && *cur != ';' && *cur != '\r') {
                    ++cur;
                }
                const char *value_end = cur;

                std::string key;
                std::string value;
                append_lower_trimmed(key, key_begin, key_end);
                append_lower_trimmed(value, value_begin, value_end);
                if (!key.empty()) {
                    extra[std::move(key)] = std::move(value);
                }

                if (cur < end && *cur == ';') {
                    ++cur;
                }
            }
        }

        cur = consume_crlf_if_any(cur);
        return { true, static_cast<uint32_t>(cur - p) };
    }

    bool HttpPacket::parse_content()
    {
        if (!is_good_)
            return false;

        if (has_body_file()) {
            return true;
        }

        if (!body_related_header_present_) {
            return true;
        }

        const std::string *ctype = get_header(http_header_key::content_type);
        if (!ctype && !is_chunked())
            return true;

        is_good_ = ContentParserFactory::get_instance()->parse_content(this);

        return is_good_;
    }

    bool HttpPacket::parse(const yuan::buffer::ByteBuffer & buff)
    {
        if (is_ok() && !is_downloading()) {
            return true;
        }

        if (is_downloading()) {
            if (!task_)
                return false;

            task_->on_data(buff);
            if (!task_->is_good()) {
                is_good_ = false;
                return false;
            }
            return true;
        }

        if (!buff.empty()) {
            input_cache_.append(buff);
        }

        return parse_input_cache();
    }

    bool HttpPacket::parse(yuan::buffer::ByteBuffer &&buff)
    {
        if (is_ok() && !is_downloading()) {
            return true;
        }

        if (is_downloading()) {
            if (!task_)
                return false;

            task_->on_data(buff);
            if (!task_->is_good()) {
                is_good_ = false;
                return false;
            }
            return true;
        }

        if (!buff.empty()) {
            if (input_cache_.empty()) {
                input_cache_ = std::move(buff);
            } else {
                input_cache_.append(buff);
            }
        }

        return parse_input_cache();
    }

    bool HttpPacket::parse_input_cache()
    {
        if (input_cache_.readable_bytes() > get_max_packet_size()) {
            LOG_ERROR("too large packet!");
            is_good_ = false;
            return false;
        }

        const int res = parser_->parse(input_cache_);
        input_cache_.compact();

        if (res < 0) {
            if (res == -1 && parser_ && !parser_->header_done() && !has_complete_http_header(input_cache_)) {
                if (input_cache_.readable_bytes() > config::max_header_length) {
                    is_good_ = false;
                    return false;
                }
                is_good_ = true;
                return false;
            }
            error_code_ = res == -2 ? ResponseCode::payload_too_large : ResponseCode::bad_request;
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

    bool HttpPacket::write(yuan::buffer::ByteBuffer & buff)
    {
        if (is_uploading()) {
            if (task_) {
                return task_->on_data(&buff);
            }
        }

        return false;
    }

    void HttpPacket::send()
    {
        pack_and_send(context_->get_connection());
    }

    bool HttpPacket::is_chunked() const
    {
        if (const auto *transfer_encoding = get_header(http_header_key::transfer_encoding)) {
            return transfer_encoding_has_chunked(*transfer_encoding);
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
        if (parser_)
            parser_->set_body_state(state);
    }

    std::string HttpPacket::get_peer_ip() const
    {
        return context_->get_connection()->get_remote_address().get_ip();
    }

    uint32_t HttpPacket::get_peer_ip_uint32() const
    {
        const auto &remote = context_->get_connection()->get_remote_address();
        if (remote.is_ipv6()) {
            return 0;
        }
        return remote.get_net_ip();
    }

    size_t HttpPacket::get_max_packet_size()
    {
        return config::max_header_length + config::client_max_content_length;
    }

    void HttpPacket::reserve_body_buffer(std::size_t size)
    {
        buffer_.reserve(size);
    }

    char *HttpPacket::body_write_ptr()
    {
        return buffer_.write_ptr();
    }

    void HttpPacket::commit_body_bytes(std::size_t size)
    {
        if (size > 0) {
            buffer_.commit(size);
        }
    }

    void HttpPacket::append_body(std::string_view text)
    {
        if (!text.empty()) {
            reserve_body_buffer(buffer_.readable_bytes() + text.size());
            buffer_.append(text);
        }
    }

    std::size_t HttpPacket::body_buffer_size() const
    {
        return buffer_.readable_bytes();
    }

    std::string HttpPacket::body_buffer_text() const
    {
        const auto span = buffer_.readable_span();
        return span.empty() ? std::string() : std::string(span.data(), span.size());
    }

    const char *HttpPacket::body_buffer_begin() const
    {
        return buffer_.readable_bytes() > 0 ? buffer_.read_ptr() : nullptr;
    }

    const char *HttpPacket::body_buffer_end() const
    {
        return buffer_.readable_bytes() > 0 ? buffer_.read_ptr() + buffer_.readable_bytes() : nullptr;
    }

    yuan::buffer::ByteBuffer HttpPacket::take_body_output_buffer()
    {
        auto body = std::move(buffer_);
        buffer_ = yuan::buffer::ByteBuffer{};
        return body;
    }

    void HttpPacket::set_body_file_path(std::filesystem::path path)
    {
        body_file_path_ = std::move(path);
        body_file_owned_ = false;
    }

    const std::filesystem::path &HttpPacket::body_file_path() const
    {
        return body_file_path_;
    }

    bool HttpPacket::has_body_file() const
    {
        return !body_file_path_.empty();
    }

    bool HttpPacket::begin_body_file_spool(std::uint32_t expected_length)
    {
        if (expected_length == 0) {
            return false;
        }
        if (body_file_stream_) {
            return true;
        }
        body_file_path_ = make_body_spool_path();
        body_file_owned_ = true;
        body_file_expected_ = expected_length;
        body_file_received_ = 0;
        body_file_stream_ = std::make_unique<std::ofstream>(body_file_path_, std::ios::binary | std::ios::trunc);
        if (!body_file_stream_->good()) {
            body_file_stream_.reset();
            body_file_path_.clear();
            body_file_expected_ = 0;
            return false;
        }
        return true;
    }

    bool HttpPacket::append_body_file_bytes(const char *data, std::size_t size)
    {
        if (!body_file_stream_ || !data || size == 0) {
            return size == 0;
        }
        body_file_stream_->write(data, static_cast<std::streamsize>(size));
        if (!body_file_stream_->good()) {
            return false;
        }
        body_file_received_ += static_cast<std::uint32_t>(size);
        if (body_file_received_ >= body_file_expected_) {
            body_file_stream_->flush();
            body_file_stream_->close();
            body_file_stream_.reset();
        }
        return true;
    }

    bool HttpPacket::body_file_spool_done() const
    {
        return body_file_expected_ > 0 && body_file_received_ >= body_file_expected_;
    }

    std::size_t HttpPacket::body_file_received() const
    {
        return body_file_received_;
    }

    void HttpPacket::pack_and_send(Connection * conn)
    {
        assert(conn);
        if (pack_header(conn)) {
            if (!buffer_.empty()) {
                conn->append_output(buffer_);
                buffer_.clear();
            }
        } else {
            is_good_ = false;
        }
        conn->flush();
    }
}
