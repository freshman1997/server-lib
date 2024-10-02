#include "client/client_session.h"
#include "net/socket/inet_address.h"
#include "common/def.h"
#include "common/file_stream_session.h"
#include "common/session.h"
#include "common/file_stream.h"
#include <iostream>

namespace net::ftp 
{
    ClientFtpSession::ClientFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent) : FtpSession(conn, app, WorkMode::client, keepUtilSent)
    {
        //info = nullptr;
    }

    ClientFtpSession::~ClientFtpSession()
    {

    }

    void ClientFtpSession::on_opened(FtpFileStreamSession *fs)
    {
        FtpSession::on_opened(fs);
    }
    
    void ClientFtpSession::on_read(Connection *conn)
    {
        // TODO 解析返回包
        
        /*
        std::string str(conn->get_input_buff()->peek(), conn->get_input_buff()->peek_end());
        std::cout << "receive: " << str << '\n';
        if (!context_.file_stream_) {
            start_file_stream({"192.168.96.1", 12124}, StreamMode::Sender);
            return;
        }
      
        if (str == "OK") {
            context_.conn_->get_output_buff()->write_string("STOR cz.mp4\r\n");
        } else {
            context_.file_manager_.set_work_filepath("/home/yuan/test");
            auto file = context_.file_manager_.get_next_file();
            file->mode_ = StreamMode::Sender;
            file->ready_ = true;
            info = file;
            set_work_file(file);
        }
        */
    }
}