#ifndef __NET_FTP_SESSION_H__
#define __NET_FTP_SESSION_H__

#include "net/base/connection/connection.h"
#include <string>
#include <unordered_map>
#include <cstdint>

namespace net::ftp 
{
    class FtpFileStream;

    enum class FtpSessionValueType : int
    {
        invalid = -1,
        int32_val,
        double_val,
        string_val,
        ptr_val,
    };

    struct FtpSessionValue
    {
        union 
        {
            int32_t         ival;
            double          dval;
            std::string    *sval;
            void           *ptr;
        } item;

        FtpSessionValueType type = FtpSessionValueType::invalid;
    };

    class FtpSessionContext
    {
    public:
        FtpSessionContext();

    public:
        bool has_password()
        {
            return !password_.empty();
        }

        FtpFileStream * get_file_stream()
        {
            return file_stream_;
        }

        bool start_file_stream_transe();

    private:
        FtpFileStream *file_stream_;
        std::string username_;
        std::string password_;
        std::unordered_map<std::string, FtpSessionValue> values;
    };

    class FtpSession
    {
    public:
        FtpSession(Connection *conn);
        ~FtpSession();

    public:
        void on_packet();

        void on_stream_close();

    public:
        Connection * get_connection()
        {
            return conn_;
        }

        bool on_login(const std::string &username, std::string &passwd);

        bool init_file_stream(const InetAddress &addr);

        bool logined()
        {
            return context_.has_password();
        }

    private:
        Connection *conn_;
        FtpSessionContext context_;
    };
}

#endif