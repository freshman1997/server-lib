#include "response_parser.h"
#include "response.h"
#include "response_code.h"
#include <cstdlib>

namespace yuan::net::http 
{
    bool HttpResponseParser::parse_header(buffer::BufferReader &reader)
    {
        if (header_state == HeaderState::init) {
            if (!parse_version(reader, ' ', 0)) {
                header_state = HeaderState::init;
                return false;
            }

            header_state = HeaderState::version_gap;
        }

        if (header_state == HeaderState::version_gap) {
            if (!parse_status(reader)) {
                header_state = HeaderState::version_gap;
                return false;
            }

            header_state = HeaderState::header_status_gap;
        }

        if (header_state == HeaderState::header_status_gap) {
            if (!parse_status_desc(reader)) {
                header_state = HeaderState::header_status_gap;
                return false;
            }

            header_state = HeaderState::header_status_desc_gap;
        }

        if (header_state == HeaderState::header_status_desc_gap) {
            if (!parse_header_keys(reader)) {
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
        if (!resp || header_state != state || reader.readable_bytes() == 0) { \
            return false; \
        }
    
    bool HttpResponseParser::parse_status(buffer::BufferReader &reader)
    {
        PRE_CHECK(HeaderState::version_gap)

        header_state = HeaderState::header_status;

        std::string status;
        char ch = reader.read_int8();
        status.push_back(ch);
        while (ch != ' ' && reader.readable_bytes()) {
            ch = reader.read_int8();
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

    bool HttpResponseParser::parse_status_desc(buffer::BufferReader &reader)
    {
        PRE_CHECK(HeaderState::header_status_gap)

        std::string data;
        char ch = reader.read_int8();
        data.push_back(ch);
        while (ch != '\r' && ch != '\n' && reader.readable_bytes()) {
            ch = reader.read_int8();
            if (ch != '\r' && ch != '\n') {
                data.push_back(ch);
            }
        }
        
        // last /n
        reader.read_int8();

        return true;
    }
}