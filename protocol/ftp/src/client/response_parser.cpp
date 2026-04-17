#include "client/response_parser.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpResponseParser::FtpResponseParser() = default;

    FtpResponseParser::~FtpResponseParser() = default;

    void FtpResponseParser::set_buff(const ::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() > 0) {
            buff_.append(buff);
        }
    }

    void FtpResponseParser::set_buff(::yuan::buffer::ByteBuffer && buff)
    {
        if (buff.readable_bytes() > 0) {
            buff_.append(buff);
            buff.clear();
        }
    }

    std::vector<FtpClientResponse> FtpResponseParser::split_responses(const std::string_view & end_with)
    {
        std::vector<FtpClientResponse> responses;
        if (buff_.readable_bytes() == 0) {
            return responses;
        }

        while (buff_.readable_bytes() >= end_with.size()) {
            const char *begin = buff_.read_ptr();
            const char *end = buff_.read_ptr() + buff_.readable_bytes();
            const char *line_end = std::search(begin, end, end_with.begin(), end_with.end());
            if (line_end == end) {
                break;
            }

            std::string line(begin, line_end);
            buff_.consume(static_cast<size_t>((line_end - begin) + end_with.size()));
            buff_.compact();
            if (line.size() < 3) {
                continue;
            }

            bool numeric = std::isdigit(static_cast<unsigned char>(line[0])) && std::isdigit(static_cast<unsigned char>(line[1])) && std::isdigit(static_cast<unsigned char>(line[2]));
            if (!numeric) {
                if (in_multi_line_) {
                    multi_line_body_.append(line);
                    multi_line_body_.append("\r\n");
                }
                continue;
            }

            int code = std::stoi(line.substr(0, 3));
            char separator = line.size() > 3 ? line[3] : ' ';

            if (in_multi_line_) {
                if (code == multi_line_code_ && separator == ' ') {
                    if (line.size() > 4) {
                        multi_line_body_.append(line.substr(4));
                    }
                    responses.push_back({ code, std::move(multi_line_body_) });
                    multi_line_body_.clear();
                    in_multi_line_ = false;
                } else {
                    multi_line_body_.append(line.size() > 4 ? line.substr(4) : line);
                    multi_line_body_.append("\r\n");
                }
            } else {
                if (separator == '-' && code != 0) {
                    in_multi_line_ = true;
                    multi_line_code_ = code;
                    if (line.size() > 4) {
                        multi_line_body_ = line.substr(4);
                        multi_line_body_.append("\r\n");
                    } else {
                        multi_line_body_.clear();
                    }
                } else {
                    std::string body;
                    if (line.size() > 4) {
                        body = line.substr(4);
                    }
                    responses.push_back({ code, std::move(body) });
                }
            }
        }

        return responses;
    }
}
