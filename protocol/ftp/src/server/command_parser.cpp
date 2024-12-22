#include "server/command_parser.h"
#include "buffer/pool.h"
#include "base/utils/string_util.h"
#include "common/def.h"

#include <cassert>

namespace yuan::net::ftp 
{
    FtpCommandParser::FtpCommandParser() : buff_(nullptr)
    {

    }

    FtpCommandParser::~FtpCommandParser()
    {
        if (buff_) {
            buffer::BufferedPool::get_instance()->free(buff_);
            buff_ = nullptr;
        }
    }

    std::vector<FtpCommand> FtpCommandParser::split_cmds(const std::string_view &endWith, const std::string &splitStr)
    {
        const char *begin = buff_->peek();
        const char *end = buff_->peek_end();
        int idx = base::util::find_first(begin, end, endWith.data());
        if (idx < 0) {
            return {};
        }

        std::vector<FtpCommand> res;
        for ( ; idx > 0 && begin <= end; ) {
            int foundIdx = base::util::find_first(begin, end, splitStr.c_str());
            bool hasArgs = true;
            if (foundIdx < 0) {
                foundIdx = idx;
                hasArgs = false;
            }

            std::string cmd = std::string(begin, begin + foundIdx);
            std::string args;
            if (hasArgs) {
                assert(begin + foundIdx + 1 < begin + idx);
                args = std::string(begin + foundIdx + 1, begin + idx);
            }

            begin += idx + endWith.size();
            idx = base::util::find_first(begin, end, endWith.data());

            res.push_back({cmd, args});
        }

        // move rest
        std::string tmp(begin, end);
        buff_->reset();
        buff_->write_string(tmp);

        return res;
    }

    void FtpCommandParser::set_buff(buffer::Buffer *buff)
    {
        if (buff_) {
            buff_->append_buffer(*buff);
            buffer::BufferedPool::get_instance()->free(buff);
        } else {
            buff_ = buff;
        }
    }

    buffer::Buffer * FtpCommandParser::get_buff()
    {
        return buff_;
    }
}