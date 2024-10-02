#ifndef _NET_FTP_COMMON_FILE_UTIL_H__
#define _NET_FTP_COMMON_FILE_UTIL_H__
#include <string>

#include "def.h"

namespace net::ftp 
{
    class FileUtil
    {
    public:
        static void list_files(const std::string &filepath, std::vector<FtpFileInfo> &dest, bool recurve = true);

        static std::vector<std::string> build_unix_file_infos(const std::vector<FileInfo> &infos);

        static bool parse_unix_file_info(const char *begin, const char *end, std::vector<FileInfo> &res);

    };
}

#endif