#include "server/command_parser.h"

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

    FtpCommandParser::FtpCommandParser() = default;

    FtpCommandParser::~FtpCommandParser() = default;

    std::vector<FtpCommand> FtpCommandParser::split_cmds(const std::string_view &endWith, const std::string &splitStr)
    {
        std::vector<FtpCommand> res;
        if (buff_.readable_bytes() == 0) {
            return res;
        }

        while (buff_.readable_bytes() >= endWith.size()) {
            const char *begin = buff_.read_ptr();
            const char *end = buff_.read_ptr() + buff_.readable_bytes();
            const char *lineEnd = std::search(begin, end, endWith.begin(), endWith.end());
            if (lineEnd == end) {
                break;
            }
            std::string line(begin, lineEnd);
            buff_.consume(static_cast<size_t>((lineEnd - begin) + endWith.size()));
            buff_.compact();
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

    void FtpCommandParser::set_buff(const ::yuan::buffer::ByteBuffer &buff)
    {
        if (buff.readable_bytes() > 0) {
            buff_.append(buff);
        }
    }

    void FtpCommandParser::set_buff(::yuan::buffer::ByteBuffer &&buff)
    {
        if (buff.readable_bytes() > 0) {
            buff_.append(buff);
            buff.clear();
        }
    }
}
