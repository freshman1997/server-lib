#include "net/ftp/session.h"

namespace net::ftp 
{
    FtpSessionContext::FtpSessionContext() : file_stream_(nullptr)
    {

    }


    FtpSession::FtpSession(Connection *conn) : conn_(conn)
    {

    }

    FtpSession::~FtpSession()
    {

    }


    bool FtpSession::on_login(const std::string &username, std::string &passwd)
    {
        return false;
    }

    bool FtpSession::init_file_stream(const InetAddress &addr)
    {
        return false;
    }
}