#include "server/command_parser.h"
#include "buffer/pool.h"

#include <algorithm>
#include <cctype>

namespace yuan::net::ftp
{
    namespace
    {
        std::string to_upper(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            return value;
        }
    }

    FtpCommandParser::FtpCommandParser() : buff_(nullptr) {}

    FtpCommandParser::~FtpCommandParser()
    {
        if (buff_) {
            buffer::BufferedPool::get_instance()->free(buff_);
        }
    }

    std::vector<FtpCommand> FtpCommandParser::split_cmds(const std::string_view &endWith, const std::string &splitStr)
    {
        std::vector<FtpCommand> res;
        if (!buff_ || buff_->readable_bytes() == 0) {
            return res;
        }

        while (buff_->readable_bytes() >= endWith.size()) {
            const char *begin = buff_->peek();
            const char *end = buff_->peek_end();
            const char *lineEnd = std::search(begin, end, endWith.begin(), endWith.end());
            if (lineEnd == end) {
                break;
            }
            std::string line(begin, lineEnd);
            buff_->add_read_index(static_cast<size_t>((lineEnd - begin) + endWith.size()));
            buff_->shink_to_fit();
            if (line.empty()) {
                continue;
            }
            auto splitPos = line.find(splitStr);
            std::string cmd = splitPos == std::string::npos ? line : line.substr(0, splitPos);
            std::string args = splitPos == std::string::npos ? std::string{} : line.substr(splitPos + splitStr.size());
            while (!args.empty() && args.front() == ' ') {
                args.erase(args.begin());
            }
            res.push_back({to_upper(std::move(cmd)), std::move(args)});
        }
        return res;
    }

    void FtpCommandParser::set_buff(buffer::Buffer *buff)
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

    buffer::Buffer *FtpCommandParser::get_buff() { return buff_; }
}
