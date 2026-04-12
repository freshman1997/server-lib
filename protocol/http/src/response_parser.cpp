#include "response_parser.h"
#include "response.h"
#include "response_code.h"
#include <cstdlib>

namespace yuan::net::http 
{
    bool HttpResponseParser::parse_header(::yuan::buffer::ByteBuffer &buff)
    {
        auto from = buff.read_offset();
        if (header_state == HeaderState::init) {
            if (!parse_version(buff, ' ', 0)) {
                buff.set_read_offset(from);
                header_state = HeaderState::init;
                return false;
            }

            header_state = HeaderState::version_gap;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::version_gap) {
            if (!parse_status(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::version_gap;
                return false;
            }

            header_state = HeaderState::header_status_gap;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::header_status_gap) {
            if (!parse_status_desc(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::header_status_gap;
                return false;
            }

            header_state = HeaderState::header_status_desc_gap;
        }

        from = buff.read_offset();
        if (header_state == HeaderState::header_status_desc_gap) {
            if (!parse_header_keys(buff)) {
                buff.set_read_offset(from);
                header_state = HeaderState::header_status_desc_gap;
                packet_->clear_header();
                return false;
            }

            header_state = HeaderState::header_end_lines;
        }

        return true;
    }

    #define PRE_CHECK(state) \
        HttpResponse *resp = static_cast<HttpResponse *>(packet_); \
        if (!resp || header_state != state || buff.readable_bytes() == 0) { \
            return false; \
        }
    
    bool HttpResponseParser::parse_status(::yuan::buffer::ByteBuffer &buff)
    {
        PRE_CHECK(HeaderState::version_gap)

        header_state = HeaderState::header_status;

        std::string status;
        char ch = buff.read_i8();
        status.push_back(ch);
        while (ch != ' ' && buff.readable_bytes()) {
            ch = buff.read_i8();
            if (ch != ' ') {
                status.push_back(ch);
            }
        }

        if (status.empty()) {
            return false;
        }

        int code = std::atoi(status.c_str());
        resp->set_response_code((ResponseCode)code);

        return true;
    }

    bool HttpResponseParser::parse_status_desc(::yuan::buffer::ByteBuffer &buff)
    {
        PRE_CHECK(HeaderState::header_status_gap)

        std::string data;
        char ch = buff.read_i8();
        data.push_back(ch);
        while (ch != '\r' && ch != '\n' && buff.readable_bytes()) {
            ch = buff.read_i8();
            if (ch != '\r' && ch != '\n') {
                data.push_back(ch);
            }
        }
        
        // last /n
        buff.read_i8();

        return true;
    }
}
