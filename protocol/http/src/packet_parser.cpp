#include "packet_parser.h"
#include "content/content_parser_factory.h"
#include "content_type.h"
#include "ops/option.h"
#include "packet.h"
#include "header_key.h"
#include "request.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace yuan::net::http
{
    namespace
    {
        constexpr uint32_t kBodyFileSpoolThreshold = 64u * 1024u;

        inline char ascii_lower_char(char ch) noexcept
        {
            return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
        }

        bool equals_ci_literal(const char *begin, std::size_t len, const char *literal) noexcept
        {
            if (!begin || !literal) {
                return false;
            }
            std::size_t i = 0;
            for (; i < len && literal[i] != '\0'; ++i) {
                if (ascii_lower_char(begin[i]) != literal[i]) {
                    return false;
                }
            }
            return i == len && literal[i] == '\0';
        }

        const char *canonical_header_key(const char *begin, std::size_t len) noexcept
        {
            switch (len) {
            case 4:
                return equals_ci_literal(begin, len, http_header_key::host) ? http_header_key::host : nullptr;
            case 6:
                return equals_ci_literal(begin, len, http_header_key::accept) ? http_header_key::accept : nullptr;
            case 10:
                if (equals_ci_literal(begin, len, http_header_key::connection)) {
                    return http_header_key::connection;
                }
                if (equals_ci_literal(begin, len, http_header_key::user_agent)) {
                    return http_header_key::user_agent;
                }
                return nullptr;
            case 14:
                if (equals_ci_literal(begin, len, http_header_key::content_type)) {
                    return http_header_key::content_type;
                }
                if (equals_ci_literal(begin, len, http_header_key::content_length)) {
                    return http_header_key::content_length;
                }
                return nullptr;
            case 15:
                return equals_ci_literal(begin, len, http_header_key::accept_encoding) ? http_header_key::accept_encoding : nullptr;
            case 17:
                return equals_ci_literal(begin, len, http_header_key::transfer_encoding) ? http_header_key::transfer_encoding : nullptr;
            default:
                return nullptr;
            }
        }

        enum class ContentLengthParseState
        {
            missing,
            valid,
            invalid
        };

        bool should_spool_request_body(HttpPacket *packet, uint32_t length)
        {
            if (!packet || packet->get_packet_type() != PacketType::request || length <= kBodyFileSpoolThreshold) {
                return false;
            }
            auto *req = static_cast<HttpRequest *>(packet);
            return req->get_method() == HttpMethod::put_;
        }

        ContentLengthParseState parse_content_length_header(const HttpPacket *packet, uint32_t &result)
        {
            result = 0;
            if (!packet) {
                return ContentLengthParseState::missing;
            }

            const std::string *length = packet->get_header(http_header_key::content_length);
            if (!length) {
                return ContentLengthParseState::missing;
            }

            const char *start = length->data();
            const char *end = start + length->size();

            while (start < end && (*start == ' ' || *start == '\t')) {
                ++start;
            }

            while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
                --end;
            }

            if (start == end) {
                return ContentLengthParseState::invalid;
            }

            for (const char *p = start; p < end; ++p) {
                if (*p < '0' || *p > '9') {
                    return ContentLengthParseState::invalid;
                }
            }

            auto[
                ptr,
                ec
            ] = std::from_chars(start, end, result);
            if (ec != std::errc{} || ptr != end) {
                return ContentLengthParseState::invalid;
            }

            return ContentLengthParseState::valid;
        }
    }

    uint32_t HttpPacketParser::get_body_length()
    {
        uint32_t result = 0;
        return parse_content_length_header(packet_, result) == ContentLengthParseState::valid ? result : 0;
    }

    bool HttpPacketParser::parse_version(::yuan::buffer::ByteBuffer & buff, char ending, char next)
    {
        if ((header_state != HeaderState::url_gap && header_state != HeaderState::init) || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::version;

        const char *data = buff.read_ptr();
        const std::size_t size = buff.readable_bytes();
        std::size_t token_len = 0;
        while (token_len < size && data[token_len] != ending) {
            ++token_len;
        }
        if (token_len >= size) {
            return false;
        }

        if (token_len < 6) {
            return false;
        }

        if (ascii_lower_char(data[0]) != 'h' ||
            ascii_lower_char(data[1]) != 't' ||
            ascii_lower_char(data[2]) != 't' ||
            ascii_lower_char(data[3]) != 'p' ||
            data[4] != '/') {
            return false;
        }

        const char *version_begin = data + 5;
        const std::size_t version_len = token_len - 5;

        std::size_t consumed = token_len + 1;
        if (next) {
            if (consumed >= size || data[consumed] != next) {
                return false;
            }
            ++consumed;
        }

        if (version_len == 3 && version_begin[0] == '1' && version_begin[1] == '.' && version_begin[2] == '0') {
            packet_->set_version(HttpVersion::v_1_0);
        } else if (version_len == 3 && version_begin[0] == '1' && version_begin[1] == '.' && version_begin[2] == '1') {
            packet_->set_version(HttpVersion::v_1_1);
        } else if (version_len == 3 && version_begin[0] == '2' && version_begin[1] == '.' && version_begin[2] == '0') {
            packet_->set_version(HttpVersion::v_2_0);
        } else if (version_len == 3 && version_begin[0] == '3' && version_begin[1] == '.' && version_begin[2] == '0') {
            packet_->set_version(HttpVersion::v_3_0);
        } else {
            return false;
        }

        buff.consume(consumed);

        return true;
    }

    bool HttpPacketParser::parse_header_keys(::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() == 0 || (header_state != HeaderState::version_newline && header_state != HeaderState::header_status_desc_gap)) {
            return false;
        }

        const std::size_t start_offset = buff.read_offset();
        const char *data = buff.read_ptr();
        const std::size_t size = buff.readable_bytes();
        std::size_t pos = 0;

        while (pos < size) {
            header_state = HeaderState::header_key;

            std::size_t line_end = pos;
            while (line_end < size && data[line_end] != '\n') {
                ++line_end;
                if (start_offset + line_end > config::max_header_length) {
                    header_state = HeaderState::too_long;
                    return false;
                }
            }
            if (line_end >= size) {
                return false;
            }

            if (line_end == pos || data[line_end - 1] != '\r') {
                return false;
            }

            const std::size_t line_len = line_end - pos - 1;
            if (line_len == 0) {
                const std::size_t consumed = line_end + 1;
                buff.consume(consumed);
                return true;
            }

            const char *line_begin = data + pos;
            const char *const line_data_end = line_begin + line_len;
            const char *const colon = static_cast<const char *>(std::memchr(line_begin, ':', line_len));
            if (!colon || colon == line_begin) {
                return false;
            }
            const std::size_t key_len = static_cast<std::size_t>(colon - line_begin);

            header_state = HeaderState::header_value;
            const char *value_begin = colon + 1;
            if (value_begin < line_data_end && std::isblank(static_cast<unsigned char>(*value_begin))) {
                ++value_begin;
            }
            std::string val(value_begin, static_cast<std::size_t>(line_data_end - value_begin));

            if (const char *canonical = canonical_header_key(line_begin, key_len)) {
                packet_->add_header(canonical, std::move(val));
            } else {
                std::string key(key_len, '\0');
                for (std::size_t i = 0; i < key_len; ++i) {
                    key[i] = ascii_lower_char(line_begin[i]);
                }
                packet_->add_header(std::move(key), std::move(val));
            }
            pos = line_end + 1;
        }

        return false;
    }

    int HttpPacketParser::parse_body(::yuan::buffer::ByteBuffer & buff, uint32_t length)
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

    bool HttpPacketParser::parse_new_line(::yuan::buffer::ByteBuffer & buff)
    {
        char ch = buff.read_i8();
        if (ch != '\r') {
            return false;
        }

        ch = buff.read_i8();
        if (ch != '\n') {
            return false;
        }

        return true;
    }

    int HttpPacketParser::parse(::yuan::buffer::ByteBuffer & buff)
    {
        if (done()) {
            body_state = BodyState::too_long;
            return -2;
        }

        if (!is_header_done()) {
            if (!parse_header(buff)) {
                return -1;
            }
        }

        if (header_state == HeaderState::too_long) {
            return -2;
        }

        if (is_header_done()) {
            bool has_content_length = packet_->get_header(http_header_key::content_length) != nullptr;
            bool has_transfer_encoding = packet_->get_header(http_header_key::transfer_encoding) != nullptr;

            if (has_content_length && has_transfer_encoding) {
                return -1;
            }

            if (has_transfer_encoding) {
                if (!packet_->is_chunked()) {
                    return -1;
                }
                body_state = BodyState::empty;
                return 1;
            }

            uint32_t length = packet_->get_body_length();
            if (packet_->get_body_length() == 0) {
                uint32_t parsed_length = 0;
                const auto length_state = parse_content_length_header(packet_, parsed_length);
                if (length_state == ContentLengthParseState::invalid) {
                    return -1;
                }
                if (length_state == ContentLengthParseState::valid) {
                    length = parsed_length;
                    packet_->set_body_length(length);
                }
            }
            if (length > 0) {
                if (should_spool_request_body(packet_, length)) {
                    if (!packet_->begin_body_file_spool(length)) {
                        body_state = BodyState::too_long;
                        return -2;
                    }
                    const auto remaining = length - static_cast<uint32_t>(packet_->body_file_received());
                    const auto to_write = (std::min)(remaining, static_cast<uint32_t>(buff.readable_bytes()));
                    if (to_write > 0) {
                        if (!packet_->append_body_file_bytes(buff.read_ptr(), to_write)) {
                            body_state = BodyState::too_long;
                            return -2;
                        }
                        buff.consume(to_write);
                    }
                    if (packet_->body_file_spool_done()) {
                        body_state = BodyState::fully;
                        return 1;
                    }
                    body_state = BodyState::partial;
                    return 0;
                }

                int res = parse_body(buff, length);
                if (res > 0) {
                    body_state = BodyState::fully;
                } else if (res == 0) {
                    body_state = BodyState::partial;
                    return 0;
                } else {
                    body_state = BodyState::too_long;
                    return -2;
                }

                auto cd = packet_->get_header(http_header_key::content_disposition);
                if (cd) {
                    if (packet_->get_packet_type() != PacketType::response) {
                        return -1;
                    }

                    std::string originName;
                    if (!parse_content_disposition(cd, originName)) {
                        return -1;
                    }

                    packet_->set_original_file_name(originName);
                    packet_->set_download_file(true);

                    return 1;
                } else {
                    auto contentType = packet_->get_header(http_header_key::content_type);
                    if (contentType && !ContentParserFactory::get_instance()->can_parse(find_content_type(*contentType))) {
                        if (packet_->get_packet_type() == PacketType::request) {
                            auto req = static_cast<HttpRequest *>(packet_);
                            std::string originName = req->get_last_uri();
                            packet_->set_download_file(true);
                            packet_->set_original_file_name(originName);
                        }
                        return 1;
                    }
                }

                return res;
            }
            body_state = BodyState::empty;
            return 1;
        }

        return is_body_done() ? 1 : 0;
    }

    bool HttpPacketParser::done() const
    {
        return is_header_done() && (is_body_done() || packet_->is_pending_large_block());
    }

    bool HttpPacketParser::parse_content_disposition(const std::string * val, std::string & originName)
    {
        if (!val || val->empty() || !val->starts_with("attachment; ")) {
            return false;
        }

        // attachment; filename=\"x5client-latest.zip\"; filename*=UTF-8''x5client-latest.zip
        body_state = BodyState::attachment;
        size_t pos = val->find("filename*=");
        if (pos != std::string::npos) {
            pos += 10; // length of "filename*="
            size_t encodePos = val->find("''", pos);
            if (encodePos == std::string::npos) {
                return false;
            }

            std::string encode = val->substr(pos, encodePos - pos);
            if (encode.empty()) {
                return false;
            }

            pos = encodePos + 2; // skip "''"
            size_t endPos = val->find(';', pos);
            if (endPos == std::string::npos) {
                endPos = val->size();
            }
            originName = val->substr(pos, endPos - pos);
        } else {
            pos = val->find("filename=");
            if (pos == std::string::npos) {
                return false;
            }
            pos += 9; // length of "filename="
            size_t endPos = val->find(';', pos);
            if (endPos == std::string::npos) {
                endPos = val->size();
            }
            originName = val->substr(pos, endPos - pos);
        }

        return true;
    }
}
