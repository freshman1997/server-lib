#ifndef __NET_FTP_COMMON_DEF_H__
#define __NET_FTP_COMMON_DEF_H__
#include <fstream>
#include <string>
#include <string_view>

#include "../../../buffer/buffer.h"
#include "response_code.h"

namespace net::ftp 
{
    extern std::string_view delimiter;
    extern std::size_t default_write_buff_size;
    extern int default_session_idle_timeout;

    enum class WorkMode : char
    {
        client = 0,
        server
    };

    enum class StreamMode : char
    {
        Sender,         // 发送端
        Receiver        // 接收端
    };

    struct FtpCommand
    {
        std::string cmd_;
        std::string args_;
    };

    struct FtpCommandResponse
    {
        FtpCommandResponse(FtpResponseCode code, const std::string &body, bool close = false) : code_(code), body_(std::move(body)), close_(close) {}
        bool close_;
        FtpResponseCode code_;
        std::string body_;
    };

    enum class FileState : char
    {
        init = 0,
        processing,
        processed
    };

    enum class FileType
    {
        normal_file = 0,
        directionary = 1,
    };

    struct FtpFileInfo
    {
        StreamMode     mode_               = StreamMode::Receiver;
        FileType       type_               = FileType::normal_file;
        FileState      state_              = FileState::init;
        bool           ready_              = false;
        std::string    origin_name_        = "";
        std::string    dest_name_          = "";
        std::size_t    file_size_          = 0;
        std::size_t    current_progress_   = 0;
        std::fstream * fstream_            = nullptr;

        ~FtpFileInfo();
        
        /**
         * @brief 根据给定数据大小，读取文件到buff中，小于则读取到文件末尾, 成功会修改 current_progress_ 的值
         *
         * @param size 读取数量
         * @param buff 缓冲区
         * @return int 成功则返回读取数量，失败返回 -1
         */
        int read_file(std::size_t size, Buffer *buff);

        /**
         * @brief 把缓冲区的数据写入目的地
         *
         * @param buff 
         * @return int 成功则返回读取数量，失败返回 -1, 成功会修改 current_progress_ 的值
         */
        int write_file(Buffer *buff);

        bool is_completed()
        {
            return state_ == FileState::processed;
        }

        std::string build_cmd_args();
    };

    enum class FileSteamState : char
    {
        init = 0,           // 刚初始化完成
        connecting,         // 正在建立连接
        connect_timeout,    // 建立连接超时
        connected,          // 连接已经建立
        connection_error,   // 连接异常
        disconnected,       // 连接断开
        processing,         // 正在处理文件流
        processed,          // 文件流已经处理完成
        file_error,         // 文件异常
        idle,               // 空闲中
        idle_timeout,       // 空闲超时
    };

    struct User
    {
        bool logined_ = false;
        bool anoyned_ = false;
        std::string username_;
        std::string password_;
    };

    enum class StreamStructure : char
    {
        normal = 0,             // 默认模式，文件被当作连续的字节流
        record,                 // 记录结构，
        page,                   // 页结构，有头部指定数据大小和页编号等
    };

    enum class TransferMode : char
    {
        stream = 0,             // 流模式，数据以字节流传输。对表示类型没有限制，可以使用记录结构
        chuck,                  // 块模式，文件以连续的带有数据头的数据块来传输，数据头包括一个计数域和描述码
        compress,               // 压缩模式，此模式下，有三种信息要发送：常规数据（以字节串发送）、压缩数据、控制信息（以两字节的转义字符传送。如果发送N>0（最多127）个字节的常规数据，这N个字节前要有一个头字节，这字节的最高位为0，其余7位代表数N）
    };

    enum class FileSystemType
    {
        ms_dos = 0,             // MS-DOS文件列表格式
        unix,                   // UNIX文件列表格式
    };

    // list 命令返回的数据格式
    
    // MS-DOS文件列表格式
    // 02-23-05 09:24AM 2245 readme.ESn
    // 05-25-04 08:56AM 19041660 VC.ESn

    // Windows自带FTP:
    // -rwxrwxrwx 1 owner group 19041660 May 25 2004 VC.ESn
    // -rwxrwxrwx 1 owner group 450 Apr 6 15:04 对话框中加入工具条.ESn

    // UNIX文件格式：
    // -rwxrw-r-- 1 user group 3014 Nov 12 14:57 cwinvnc337.ESn
    // -rwxrw-r-- 1 user group 20480 Mar 3 11:25 inmcsvr更新说明.ESn
    // -rwxrw-r-- 1 user group 450 Apr 13 11:39 对话框中加入工具条.ESn

    struct FileInfo
    {
        int grant_;     // 权限
        std::size_t size_;
        time_t date_;
        std::string group_;
        std::string user_;
        std::string file_name_;
    };
}

#endif