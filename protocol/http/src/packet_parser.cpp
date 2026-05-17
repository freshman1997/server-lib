#include "packet_parser.h"
#include "content/content_parser_factory.h"
#include "content_type.h"
#include "ops/option.h"
#include "packet.h"
#include "header_key.h"
#include "request.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace yuan::net::http
{
    namespace
    {
        constexpr uint32_t kBodyFileSpoolThreshold = 64u * 1024u;

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

        std::string version;
        char ch = buff.read_i8();
        version.push_back(ch);
        while (ch != ending && buff.readable_bytes()) {
            ch = buff.read_i8();
            if (ch != ending) {
                version.push_back(ch);
            }
        }

        if (next) {
            if (buff.readable_bytes() == 0 || buff.read_i8() != next) {
                return false;
            }
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
            packet_->set_version(HttpVersion::v_1_0);
        } else if (v == "1.1") {
            packet_->set_version(HttpVersion::v_1_1);
        } else if (v == "2.0") {
            packet_->set_version(HttpVersion::v_2_0);
        } else if (v == "3.0") {
            packet_->set_version(HttpVersion::v_3_0);
        } else {
            return false;
        }

        return true;
    }

    bool HttpPacketParser::parse_header_keys(::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() == 0 || (header_state != HeaderState::version_newline && header_state != HeaderState::header_status_desc_gap)) {
            return false;
        }

        while (buff.readable_bytes()) {
            header_state = HeaderState::header_key;
            std::string key;
            char ch = buff.read_i8();
            if (ch == '\r') {
                break;
            }

            while (ch != ':' && buff.readable_bytes()) {
                key.push_back(std::tolower(ch));
                if (ch != ':') {
                    ch = buff.read_i8();
                }

                if (buff.read_offset() > config::max_header_length) {
                    header_state = HeaderState::too_long;
                    return false;
                }
            }

            if (key.empty()) {
                return false;
            }

            header_state = HeaderState::header_value;
            std::string val;
            ch = buff.read_i8();
            if (!std::isblank(ch)) {
                val.push_back(ch);
            }

            while (ch != '\r' && buff.readable_bytes()) {
                ch = buff.read_i8();
                if (ch != '\r') {
                    val.push_back(ch);
                }

                if (buff.read_offset() > config::max_header_length) {
                    header_state = HeaderState::too_long;
                    return false;
                }
            }

            if (buff.readable_bytes() == 0 || buff.read_i8() != '\n') {
                return false;
            }

            packet_->add_header(std::move(key), std::move(val));
        }

        if (buff.readable_bytes() == 0 || buff.read_i8() != '\n') {
            return false;
        }

        return true;
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
