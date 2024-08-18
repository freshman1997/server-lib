#include "net/http/packet_parser.h"
#include "net/http/ops/option.h"
#include "net/http/packet.h"
#include "net/http/header_key.h"


namespace net::http 
{
    uint32_t HttpPacketParser::get_body_length()
    {
        const std::string *length = packet_->get_header(http_header_key::content_length);
        if (!length) {
            return 0;
        }

        return std::atoi(length->c_str());
    }

    bool HttpPacketParser::parse_version(Buffer &buff, char ending, char next)
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

    bool HttpPacketParser::parse_header_keys(Buffer &buff)
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

    int HttpPacketParser::parse_body(Buffer &buff, uint32_t length)
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

    bool HttpPacketParser::parse_new_line(Buffer &buff)
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

    int HttpPacketParser::parse(Buffer &buff)
    {
        Buffer *useBuff = &buff;
        if (packet_->get_buff()->readable_bytes() > 0) {
            packet_->get_buff()->append_buffer(buff);
            useBuff = packet_->get_buff();
        }
        
        if (!is_header_done()) {
            parse_header(*useBuff);
        }

        if (is_header_done()) {
            uint32_t length = packet_->get_body_length() ? packet_->get_body_length() : get_body_length();
            if (length > 0) {
                if (packet_->get_body_length() == 0) {
                    packet_->set_body_length(length);
                }

                int res = parse_body(*useBuff, length);
                if (res > 0) {
                    body_state = BodyState::fully;
                } else if (res == 0) {
                    if (packet_->get_buff()->readable_bytes() == 0) {
                        packet_->get_buff()->resize(length);
                        packet_->get_buff()->append_buffer(*useBuff);
                    }
                    body_state = BodyState::partial;
                }
                return res;
            } else {
                body_state = BodyState::empty;
                return 1;
            }
        }

        return is_body_done();
    }
}