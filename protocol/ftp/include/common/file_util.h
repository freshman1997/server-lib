#ifndef NET_FTP_COMMON_FILE_UTIL_H
#define NET_FTP_COMMON_FILE_UTIL_H
#include <string>

#include "def.h"

namespace yuan::net::ftp 
{
    class FileUtil
    {
    public:
        static void list_files(const std::string &filepath, std::vector<FtpFileInfo> &dest, bool recurve = true);
    };
}

#endif