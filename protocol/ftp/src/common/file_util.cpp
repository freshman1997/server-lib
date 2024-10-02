#include "common/file_util.h"
#include "common/def.h"

#ifdef WIN32
#include <windows.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#elif defined __linux__
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#elif defined __APPLE__

#endif

namespace net::ftp 
{
    void FileUtil::list_files(const std::string &filepath, std::vector<FtpFileInfo> &dest, bool recurve)
    {
    #ifdef WIN32
        if( (_access(filepath.c_str(), 0)) == -1 ) {
            return;
        }

        struct _stat buf;
        _stat(filepath.c_str(), &buf);
        if (_S_IFREG & buf.st_mode) {
            FtpFileInfo info;
            info.origin_name_ = filepath;
            info.file_size_ = buf.st_size;
            dest.push_back(info);
            return;
        }

        intptr_t hFile = 0;
        struct _finddata_t fileInfo;  //用来存储文件信息的结构体    
        std::string p;
        if ((hFile = _findfirst(p.assign(filepath).append("\\*").c_str(), &fileInfo)) != -1)  //第一次查找  
        {
            do {
                if ((fileInfo.attrib &  _A_SUBDIR)) {
                    if (recurve && strcmp(fileInfo.name, ".") != 0 && strcmp(fileInfo.name, "..") != 0)  //进入文件夹查找  
                    {
                        list_files(p.assign(filepath).append("\\").append(fileInfo.name), dest);
                    }
                } else {
                    FtpFileInfo info;
                    info.origin_name_ = filepath + "/" + fileInfo.name;
                    info.file_size_ = fileInfo.size;
                    dest.push_back(info);
                }
            } while (_findnext(hFile, &fileInfo) == 0);

            _findclose(hFile); //结束查找  
        }
    #elif defined __linux__
        struct stat statbuf;
        int res = -1;
        res = lstat(filepath.c_str(), &statbuf);//获取linux操作系统下文件的属性
        //参数1是传入参数，填写文件的绝对路径 | 参数2是传出参数,一个struct stat类型的结构体指针

        if (0 != res) {
            return;
        }

        if (!S_ISDIR(statbuf.st_mode)) {
            FtpFileInfo info;
            info.origin_name_ = filepath;
            info.file_size_ = statbuf.st_size;
            dest.push_back(info);
        } else {
            DIR *dir;
            struct dirent *pDir;
            if((dir = opendir(filepath.c_str())) == NULL){
                return;
            }

            while((pDir = readdir(dir)) != NULL) {
                if(strcmp(pDir->d_name, ".") == 0 || strcmp(pDir->d_name,"..") == 0){
                    continue;
                } else if(pDir->d_type == 8) { // 文件
                    FtpFileInfo info;
                    info.origin_name_ = filepath + "/" + pDir->d_name;
                    res = lstat(info.origin_name_.c_str(), &statbuf);
                    if (0 != res) {
                        return;
                    }
                    
                    info.file_size_ = statbuf.st_size;
                    dest.push_back(info);
                } else if(pDir->d_type == 10){
                    continue;
                } else if(pDir->d_type == 4) { // 子目录
                    if (recurve) {
                        std::string strNextdir = filepath + "/" + pDir->d_name;
                        list_files(strNextdir, dest);
                    }
                }
            }
            closedir(dir);
        }
    #endif
    }

    std::vector<std::string> FileUtil::build_unix_file_infos(const std::vector<FileInfo> &infos)
    {
        // TODO
        return {};
    }

    bool FileUtil::parse_unix_file_info(const char *begin, const char *end, std::vector<FileInfo> &res)
    {
        return false;
    }
}