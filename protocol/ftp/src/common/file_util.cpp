#include "common/file_util.h"
#include <filesystem>

namespace yuan::net::ftp
{
    void FileUtil::list_files(const std::string &filepath, std::vector<FtpFileInfo> &dest, bool recurve)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path(filepath);
        if (!fs::exists(path, ec)) {
            return;
        }
        auto appendFile = [&](const fs::path &filePath) { FtpFileInfo info; info.origin_name_ = filePath.generic_string(); info.dest_name_ = filePath.filename().generic_string(); info.file_size_ = static_cast<std::size_t>(fs::file_size(filePath, ec)); dest.push_back(info); };
        if (fs::is_regular_file(path, ec)) {
            appendFile(path);
            return;
        }
        if (!recurve) {
            for (const auto &entry : fs::directory_iterator(path, ec)) {
                if (entry.is_regular_file()) {
                    appendFile(entry.path());
                }
            }
            return;
        }
        for (const auto &entry : fs::recursive_directory_iterator(path, ec)) {
            if (entry.is_regular_file()) {
                appendFile(entry.path());
            }
        }
    }
}
