#ifndef __NET_FTP_COMMON_DEF_H__
#define __NET_FTP_COMMON_DEF_H__
#include <string>
#include <string_view>

#include "../../../buffer/buffer.h"

namespace net::ftp 
{
    extern const std::string_view login;

    enum class StreamMode : char
    {
        Sender,         // 发送端
        Receiver        // 接收端
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

    struct FileInfo
    {
        FileType    type_;
        FileState   state_;
        std::string origin_name_;
        std::string dest_name_;
        std::size_t file_size_;
        std::size_t current_progress_;

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
}

#endif