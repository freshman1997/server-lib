#include "packet_parser.h"
#include "content/content_parser_factory.h"
#include "content_type.h"
#include "ops/option.h"
#include "packet.h"
#include "header_key.h"
#include "request.h"


namespace yuan::net::http 
{
    uint32_t HttpPacketParser::get_body_length()
    {
        const std::string *length = packet_->get_header(http_header_key::content_length);
        if (!length) {
            return 0;
        }

        return std::atoi(length->c_str());
    }

    bool HttpPacketParser::parse_version(buffer::Buffer &buff, char ending, char next)
    {
        if ((header_state != HeaderState::url_gap && header_state != HeaderState::init) || buff.readable_bytes() == 0) {
            return false;
        }

        header_state = HeaderState::version;

        std::string version;
        char ch = buff.read_int8();
        version.push_back(ch);
        while (ch != ending && buff.readable_bytes()) {
            ch = buff.read_int8();
            if (ch != ending) {
                version.push_back(ch);
            }
        }

        if (next && buff.read_int8() != next) {
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

    bool HttpPacketParser::parse_header_keys(buffer::Buffer &buff)
    {
        if (buff.readable_bytes() == 0 || (header_state != HeaderState::version_newline && header_state != HeaderState::header_status_desc_gap)) {
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

                if (buff.get_read_index() > config::max_header_length) {
                    header_state = HeaderState::too_long;
                    return false;
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

                if (buff.get_read_index() > config::max_header_length) {
                    header_state = HeaderState::too_long;
                    return false;
                }
            }

            if (buff.read_int8() != '\n') {
                return false;
            }

            packet_->add_header(key, val);
        }

        if (buff.read_int8() != '\n') {
            return false;
        }

        return true;
    }

    int HttpPacketParser::parse_body(buffer::Buffer &buff, uint32_t length)
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

    bool HttpPacketParser::parse_new_line(buffer::Buffer &buff)
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

    int HttpPacketParser::parse(buffer::Buffer &buff)
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
            uint32_t length = packet_->get_body_length() ? packet_->get_body_length() : get_body_length();
            if (length > 0) {
                if (packet_->get_body_length() == 0) {
                    packet_->set_body_length(length);
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
                        auto req = static_cast<HttpRequest *>(packet_);
                        std::string originName = req->get_last_uri();
                        packet_->set_download_file(true);
                        packet_->set_original_file_name(originName);
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

    bool HttpPacketParser::parse_content_disposition(const std::string *val, std::string &originName)
    {
        if (!val || val->empty() || !val->starts_with("attachment; ")) {
            return false;
        }

        // attachment; filename=\"x5client-latest.zip\"; filename*=UTF-8''x5client-latest.zip
        body_state = BodyState::attachment;
        size_t pos = val->find("filename*=");
        if (pos != std::string::npos) {
            pos += 10; // length of "filename*="
            size_t encodePos = val->find_first_of("''", pos);
            if (encodePos == std::string::npos) {
                return false;
            }

            std::string encode = val->substr(pos, encodePos - pos);
            if (encode.empty()) {
                return false; // invalid encoding
            }

            pos = encodePos + 2; // skip "''"
            size_t endPos = val->find_first_of("; ", pos);
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
            size_t endPos = val->find_first_of("; ", pos);
            if (endPos == std::string::npos) {
                endPos = val->size();
            }
            originName = val->substr(pos, endPos - pos);
        }

        return true;
    }
}