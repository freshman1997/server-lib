#include "server/server_file_stream.h"
#include "common/file_stream.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/acceptor/tcp_acceptor.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "common/session.h"
#include "handler/ftp_app.h"
#include "net/runtime/network_runtime.h"
#include "native_platform.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <memory>
#include "logger.h"

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
                    sockaddr_storage peer_storage{};
                    memset(&peer_storage, 0, sizeof(sockaddr_storage));
                    int conn_fd = socket_->accept(peer_storage);
                    if (conn_fd < 0) {
                        const int accept_error = yuan::app::GetLastNativeError();
#ifdef _DEBUG
                        bool ignorable_error = yuan::app::IsNativeRetryableError(accept_error) ||
                                               yuan::app::ClassifyNativeError(accept_error) == yuan::app::NativeError::connection_aborted;
#ifdef EPROTO
                        ignorable_error = ignorable_error || accept_error == EPROTO;
#endif
                        if (!ignorable_error) {
                            LOG_WARN("error connection {} ({})", accept_error, yuan::app::DescribeNativeError(accept_error));
                        }
#endif
                        break;
                    }

                    InetAddress peer_addr(peer_storage);

                    std::shared_ptr<SSLHandler> sslHandler = nullptr;
                    if (ssl_module_) {
                        sslHandler = ssl_module_->create_handler(conn_fd, SSLHandler::SSLMode::acceptor_);
                        if (!sslHandler) {
                            if (auto msg = ssl_module_->get_error_message()) {
                                LOG_ERROR("ssl error: {}", msg->c_str());
                            }
                            ::close(conn_fd);
                            continue;
                        }
                    }

                    auto conn = create_stream_connection(peer_addr.get_ip(), peer_addr.get_port(), conn_fd);
                    conn->set_event_handler(handler_);
                    if (conn_handler_owner_) {
                        conn->set_connection_handler(conn_handler_owner_);
                    }

                    if (sslHandler) {
                        conn->set_ssl_handler(sslHandler);
                        conn->set_ssl_handshaking(true);
                        int ret = sslHandler->ssl_init_action();
                        if (ret > 0) {
                            conn->set_ssl_handshaking(false);
                        } else if (sslHandler->ssl_want_write()) {
                            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                                if (auto *channel = stream->stream_channel()) {
                                    channel->enable_write();
                                    if (handler_) {
                                        handler_->update_channel(channel);
                                    }
                                }
                            }
                        } else if (!sslHandler->ssl_want_read() && !sslHandler->ssl_want_write()) {
                            if (auto msg = ssl_module_->get_error_message()) {
                                LOG_ERROR("ssl handshake error: {}", msg->c_str());
                            }
                            conn->set_ssl_handshaking(false);
                            conn->abort();
                            continue;
                        }
                    }

                    if (auto *loop = dynamic_cast<net::EventLoop *>(handler_)) {
                        loop->on_new_connection(conn);
                    } else if (handler_) {
                        handler_->on_new_connection(conn);
                    }
                }
            }
        };
    }

    ServerFtpFileStream::ServerFtpFileStream(FtpSession * session)
        : FtpFileStream(session)
    {
    }

    ServerFtpFileStream::~ServerFtpFileStream()
    {
        if (acceptor_) {
            acceptor_->close();
            acceptor_.reset();
        }
    }

    void ServerFtpFileStream::on_connected(Connection & conn)
    {
        FtpFileStream::on_connected(conn);
    }

    bool ServerFtpFileStream::start(const InetAddress & addr)
    {
        assert(session_);
        if (acceptor_) {
            return true;
        }

        auto sock = std::make_unique<Socket>(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            LOG_ERROR("cant create socket file descriptor!");
            return false;
        }

        if (!sock->bind()) {
            LOG_ERROR("cant bind port: {}!", addr.get_port());
            return false;
        }

        acceptor_.reset(new PassiveTcpAcceptor(sock.release()));
        if (!acceptor_->listen()) {
            LOG_ERROR("cant listen on port: {}!", addr.get_port());
            acceptor_.reset();
            return false;
        }

        auto *runtime = session_->get_app()->get_runtime();
        assert(runtime);

        runtime->register_acceptor(acceptor_, make_non_owning_handler(this));

        return true;
    }

    void ServerFtpFileStream::quit(const InetAddress & addr)
    {
        FtpFileStream::quit(addr);
    }
}
