#ifndef _NET_FTP_COMMON_FILE_UTIL_H__
#define _NET_FTP_COMMON_FILE_UTIL_H__
#include <string>

#include "def.h"

namespace net::ftp 
{
    class FileUtil
    {
    public:
        static void list_files(const std::string &filepath, std::vector<FileInfo> &dest);
    };
}

#endif