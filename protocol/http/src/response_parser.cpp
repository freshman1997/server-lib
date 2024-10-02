#include "response_parser.h"
#include "response.h"

namespace net::http 
{
    bool HttpResponseParser::parse_header(Buffer &buff)
    {
        int from = buff.get_read_index();
        if (header_state == HeaderState::init) {
            if (!parse_version(buff, ' ', 0)) {
                buff.reset_read_index(from);
                header_state = HeaderState::init;
                return false;
            }

            header_state = HeaderState::version_gap;
        }

        from = buff.get_read_index();
        if (header_state == HeaderState::version_gap) {
            if (!parse_status(buff)) {
                buff.reset_read_index(from);
                header_state = HeaderState::version_gap;
                return false;
            }

            header_state = HeaderState::header_status_gap;
        }

        from = buff.get_read_index();
        if (header_state == HeaderState::header_status_gap) {
            if (!parse_status_desc(buff)) {
                buff.reset_read_index(from);
                header_state = HeaderState::header_status_gap;
                return false;
            }

            header_state = HeaderState::header_status_desc_gap;
        }

        from = buff.get_read_index();
        if (header_state == HeaderState::header_status_desc_gap) {
            if (!parse_header_keys(buff)) {
                buff.reset_read_index(from);
                header_state = HeaderState::header_status_desc_gap;
                return false;
            }

            header_state = HeaderState::header_end_lines;
        }

        return true;
    }

    #define PRE_CHECK(state) \
        HttpResponse *resp = dynamic_cast<HttpResponse *>(packet_); \
        if (!resp || header_state != state || buff.readable_bytes() == 0) { \
            return false; \
        }
    
    bool HttpResponseParser::parse_status(Buffer &buff)
    {
        PRE_CHECK(HeaderState::version_gap)

        header_state = HeaderState::header_status;

        std::string status;
        char ch = buff.read_int8();
        status.push_back(ch);
        while (ch != ' ' && buff.readable_bytes()) {
            ch = buff.read_int8();
            if (ch != ' ') {
                status.push_back(ch);
            }
        }
        return true;
    }

    bool HttpResponseParser::parse_status_desc(Buffer &buff)
    {
        PRE_CHECK(HeaderState::header_status_gap)

        std::string data;
        char ch = buff.read_int8();
        data.push_back(ch);
        while (ch != '\r' && ch != '\n' && buff.readable_bytes()) {
            ch = buff.read_int8();
            if (ch != '\r' && ch != '\n') {
                data.push_back(ch);
            }
        }
        
        // last /n
        buff.read_int8();

        return true;
    }
}