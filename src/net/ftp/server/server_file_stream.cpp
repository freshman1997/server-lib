#include "net/ftp/server/server_file_stream.h"
#include "net/ftp/common/file_stream.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/base/socket/inet_address.h"
#include "net/base/socket/socket.h"
#include "net/ftp/common/session.h"
#include "net/ftp/handler/ftp_app.h"

#include <cassert>
#include <iostream>

namespace net::ftp 
{
    ServerFtpFileStream::ServerFtpFileStream(FtpSession *session) : FtpFileStream(session)
    {
        acceptor_ = nullptr;
    }

    ServerFtpFileStream::~ServerFtpFileStream()
    {
        if (acceptor_) {
            acceptor_->close();
            acceptor_ = nullptr;
        }
    }

    void ServerFtpFileStream::on_connected(Connection *conn)
    {
        FtpFileStream::on_connected(conn);
    }

    bool ServerFtpFileStream::start(const InetAddress &addr)
    {
        assert(session_);
        if (acceptor_) {
            return true;
        }

        Socket *sock = new Socket("", addr.get_port());
        if (!sock->valid()) {
            std::cerr << "cant create socket file descriptor!\n";
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            std::cerr << "cant bind port: " << addr.get_port() << "!\n";
            delete sock;
            return false;
        }

        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << "cant listen on port: " << addr.get_port() << "!\n";
            delete acceptor_;
            acceptor_ = nullptr;
            return false;
        }

        auto evHandler = session_->get_app()->get_event_handler();
        assert(evHandler);
        
        acceptor_->set_event_handler(evHandler);
        acceptor_->set_connection_handler(this);

        return true;
    }

    void ServerFtpFileStream::quit(const InetAddress &addr)
    {
        file_stream_sessions_.erase(addr);
        if (file_stream_sessions_.empty()) {
            delete this;
        }
    }
}