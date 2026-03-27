#include "client/client_file_stream.h"
#include "common/file_stream.h"
#include "common/file_stream_session.h"
#include "common/session.h"
#include "handler/ftp_app.h"
#include "net/connection/tcp_connection.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"

#include <cassert>
#include <iostream>

namespace yuan::net::ftp
{
    ClientFtpFileStream::ClientFtpFileStream(FtpSession *session) : FtpFileStream(session) {}
    ClientFtpFileStream::~ClientFtpFileStream() {}

    bool ClientFtpFileStream::start(const InetAddress &addr)
    {
        net::Socket *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            std::cerr << "create socket fail!!\n";
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cerr << " connect failed " << std::endl;
            return false;
        }

        auto *conn = new TcpConnection(sock);
        conn->set_connection_handler(this);

        auto evLoop = session_->get_app()->get_event_handler();
        assert(evLoop);
        conn->set_event_handler(evLoop);
        return true;
    }

    void ClientFtpFileStream::quit(const InetAddress &addr)
    {
        FtpFileStream::quit(addr);
    }
}
