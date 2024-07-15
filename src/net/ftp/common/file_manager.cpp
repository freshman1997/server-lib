#include "net/ftp/common/file_manager.h"
#include "net/ftp/common/file_util.h"

namespace net::ftp 
{
    FileManager::FileManager()
    {
        init();
    }

    FileManager::~FileManager()
    {
        
    }

    void FileManager::init()
    {
        file_infos_.clear();
        cur_idx_ = -1;
    }

    void FileManager::set_work_filepath(const std::string &path)
    {
        init();
        FileUtil::list_files(path, file_infos_);
    }

    bool FileManager::is_completed()
    {
        return !file_infos_.empty() && cur_idx_ + 1 >= file_infos_.size();
    }

    FileInfo * FileManager::get_next_file()
    {
        if (file_infos_.empty() || cur_idx_ + 1 >= file_infos_.size()) {
            return nullptr;
        }
        return &file_infos_[++cur_idx_];
    }

    void FileManager::add_file(const FileInfo &info)
    {
        file_infos_.push_back(info);
    }
}