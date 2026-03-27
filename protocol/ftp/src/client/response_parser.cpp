#include "client/response_parser.h"
#include "buffer/pool.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    FtpResponseParser::FtpResponseParser() : buff_(nullptr) {}

    FtpResponseParser::~FtpResponseParser()
    {
        if (buff_) {
            buffer::BufferedPool::get_instance()->free(buff_);
            buff_ = nullptr;
        }
    }

    void FtpResponseParser::set_buff(buffer::Buffer *buff)
    {
        if (!buff) {
            return;
        }
        if (buff_) {
            buff_->append_buffer(*buff);
            buffer::BufferedPool::get_instance()->free(buff);
        } else {
            buff_ = buff;
        }
    }

    std::vector<FtpClientResponse> FtpResponseParser::split_responses(const std::string_view &end_with)
    {
        std::vector<FtpClientResponse> responses;
        if (!buff_ || buff_->readable_bytes() == 0) {
            return responses;
        }

        while (buff_->readable_bytes() >= end_with.size()) {
            const char *begin = buff_->peek();
            const char *end = buff_->peek_end();
            const char *line_end = std::search(begin, end, end_with.begin(), end_with.end());
            if (line_end == end) {
                break;
            }

            std::string line(begin, line_end);
            buff_->add_read_index(static_cast<size_t>((line_end - begin) + end_with.size()));
            buff_->shink_to_fit();
            if (line.size() < 3) {
                continue;
            }

            bool numeric = std::isdigit(static_cast<unsigned char>(line[0]))
                && std::isdigit(static_cast<unsigned char>(line[1]))
                && std::isdigit(static_cast<unsigned char>(line[2]));
            if (!numeric) {
                continue;
            }

            int code = std::stoi(line.substr(0, 3));
            std::string body;
            if (line.size() > 4) {
                body = line.substr(4);
            }
            responses.push_back({code, std::move(body)});
        }

        return responses;
    }
}
