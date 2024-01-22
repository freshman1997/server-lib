#include <iostream>
#include <memory>
#include <unistd.h>

#include "buffer/buffer.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/event/event_loop.h"
#include "net/http/header_key.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
#include "net/poller/epoll_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/connection/tcp_connection.h"

namespace net::http
{
    HttpServer::HttpServer()
    {
        file_.open("/home/yuan/Desktop/cz");
        if (!file_.good()) {
            std::cout << "open file fail!\n";
            /*if (handler_) {
                handler_->on_close(this);
                std::cout << "cant open file!!!\n";
            }*/
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            std::cout << "open file fail1!\n";

            /*if (handler_) {
                handler_->on_close(this);
                std::cout << "cant open file!!!\n";
            }*/
        }

        file_.clear();
        file_.seekg(0, std::ios::beg);
        buff1_ = new char[1024 * 1024 * 2];
    }

    HttpServer::~HttpServer()
    {

    }

    void HttpServer::on_connected(TcpConnection *conn)
    {

    }

    void HttpServer::on_error(TcpConnection *conn)
    {

    }

    void HttpServer::on_read(TcpConnection *conn)
    {
        HttpRequest req_;
        if (!req_.parse_header(*conn->get_input_stream().get())) {
            std::cout << "parse fail!!" << std::endl;
            conn->close();
        } else {
            if (req_.get_url_domain().size() > 1 && req_.get_url_domain()[1] == "movie") {
                const std::string *range = req_.get_header(net::http::http_header_key::range);
                long long offset = 0;

                if (range) {
                    size_t pos = range->find_first_of("=");
                    if (std::string::npos == pos) {
                        conn->close();
                        return;
                    }

                    size_t pos1 = range->find_first_of("-");
                    offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
                }
                
                //Buffer buff(1024 * 1024);
                file_.seekg(offset, std::ios::beg);
                file_.read(buff1_, 1024 * 1024 * 2);
                size_t r = file_.gcount();
                //buff.fill(r);
                //buff.rewind();

                if (offset > 0) {
                    std::string resp = "HTTP/1.1 206 Partial Content\r\n";
                    resp.append("Content-Type: video/mp4\r\n");
                    resp.append("Content-Range: bytes ")
                        .append(std::to_string(offset))
                        .append("-")
                        .append(std::to_string(length_ - 1))
                        .append("/")
                        .append(std::to_string(length_))
                        .append("\r\n");

                    resp.append("Content-length: ").append(std::to_string(r)).append("\r\n\r\n");
                    ::write(conn->get_fd(), resp.c_str(), resp.size());

                    /*
                    Buffer buff1;
                    buff1.write_string(resp);
                    conn->send(&buff1);
                    */

                    std::cout << "offset: " << offset << ", length: " << r << ", size: " << length_ << std::endl;
                } else {
                    std::string resp = "HTTP/1.1 206 Partial Content\r\n";
                    resp.append("Content-Type: video/mp4\r\n");
                    //resp.append("Accept-Ranges: bytes\r\n");
                    resp.append("Content-Range: bytes ")
                        .append(std::to_string(offset))
                        .append("-")
                        .append(std::to_string(length_ - 1))
                        .append("/")
                        .append(std::to_string(length_))
                        .append("\r\n");
                    resp.append("Content-length: ").append(std::to_string(r)).append("\r\n\r\n");
                    //resp.append("Content-Disposition: attachment; filename=The.Shawshank.Redemption.1994.1080p.BluRay.H264.AAC-RARBG.mp4\r\n");
                    //Buffer buff1;
                    //buff1.write_string(resp);
                    //conn->send(&buff1);

                    ::write(conn->get_fd(), resp.c_str(), resp.size());
                }

                //conn->send(&buff);
                ::write(conn->get_fd(), buff1_, r);

                if (file_.eof()) {
                    file_.clear();
                }

                file_.seekg(0, std::ios::beg);
                return;
            }

            std::cout << "content length:" << req_.header_exists(net::http::http_header_key::content_length) << std::endl;
            std::string msg = "你好，世界！！";
            std::string repsonse = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
            Buffer buff1;
            buff1.write_string(repsonse);
            conn->send(&buff1);
        }
    }

    void HttpServer::on_wirte(TcpConnection *conn)
    {

    }

    void HttpServer::on_close(TcpConnection *conn)
    {
        event_loop_->on_close(conn);
    }

    bool HttpServer::init(int port)
    {
        net::Socket *sock = new net::Socket("", port);
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_reuse(true);
        if (!sock->bind()) {
            std::cout << " bind failed " << std::endl;
            return false;
        }

        sock->set_none_block(true);
        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << " listen failed " << std::endl;
            return false;
        }

        return true;
    }

    void HttpServer::serve()
    {
        net::EpollPoller poller;
        timer::WheelTimerManager manager;
        net::EventLoop loop(&poller, &manager, acceptor_);
        acceptor_->set_handler(&loop);
        this->event_loop_ = &loop;
        loop.set_tcp_handler(this);
        loop.loop();
    }

    void HttpServer::stop()
    {
        event_loop_->quit();
    }
}