#ifndef _NET_FTP_CLIENT_FILE_MANAGER_H__
#define _NET_FTP_CLIENT_FILE_MANAGER_H__
#include <vector>
#include "common/def.h"

namespace yuan::net::ftp 
{
    class FileManager
    {
    public:
        FileManager();
        ~FileManager();

    public:
        void set_work_filepath(const std::string &path);

        bool is_completed();

        FtpFileInfo * get_next_file();

        void add_file(const FtpFileInfo &info);

        void reset()
        {
            init();
        }

    private:
        void init();

    private:
        int cur_idx_;
        std::vector<FtpFileInfo> file_infos_;
    };
}

#endif