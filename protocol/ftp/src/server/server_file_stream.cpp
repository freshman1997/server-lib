#include "server/server_file_stream.h"
#include "common/file_stream.h"
#include "net/acceptor/tcp_acceptor.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/connection/tcp_connection.h"
#include "common/session.h"
#include "handler/ftp_app.h"

#include <cassert>
#include <cstring>
#include <iostream>

#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#endif

namespace yuan::net::ftp
{
    namespace
    {
        class PassiveTcpAcceptor : public net::TcpAcceptor
        {
        public:
            using net::TcpAcceptor::TcpAcceptor;

            void on_read_event() override
            {
                assert(socket_);

                while (true) {
                    sockaddr_in peer_addr{};
                    memset(&peer_addr, 0, sizeof(sockaddr_in));
                    int conn_fd = socket_->accept(peer_addr);
                    if (conn_fd < 0) {
                    #ifdef _DEBUG
                        if (errno != EAGAIN && errno != ECONNABORTED && errno != EPROTO && errno != EINTR) {
                            std::cerr << "error connection " << errno << std::endl;
                        }
                    #endif
                        break;
                    }

                    std::shared_ptr<SSLHandler> sslHandler = nullptr;
                    if (ssl_module_) {
                        sslHandler = ssl_module_->create_handler(conn_fd, SSLHandler::SSLMode::acceptor_);
                        if (!sslHandler || sslHandler->ssl_init_action() <= 0) {
                            if (auto msg = ssl_module_->get_error_message()) {
                                std::cerr << "ssl error: " << msg->c_str() << std::endl;
                            }
                            ::close(conn_fd);
                            return;
                        }
                    }

                    auto *conn = new TcpConnection(::inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port), conn_fd);
                    conn->set_event_handler(handler_);
                    conn->set_connection_handler(conn_handler_);
                    if (sslHandler) {
                        conn->set_ssl_handler(sslHandler);
                    }
                    handler_->on_new_connection(conn);
                }
            }
        };
    }

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

        acceptor_ = new PassiveTcpAcceptor(sock);
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
        FtpFileStream::quit(addr);
    }
}
