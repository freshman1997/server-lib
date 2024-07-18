#include "net/ftp/client/client_session.h"
#include <iostream>

namespace net::ftp 
{
    ClientFtpSession::ClientFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::client, keepUtilSent)
    {

    }

    ClientFtpSession::~ClientFtpSession()
    {

    }
    
    void ClientFtpSession::on_read(Connection *conn)
    {
        std::string str(conn->get_input_buff()->peek(), conn->get_input_buff()->peek_end());
        std::cout << ">>> " << str << '\n';
        conn->get_output_buff()->write_string("RETR");
    }
}