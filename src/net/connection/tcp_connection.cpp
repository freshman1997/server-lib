#include "net/connection/tcp_connection.h"
#include "net/connection/connection.h"
#include "net/handler/accept_handler.h"
#include "net/http/header_key.h"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{
    TcpConnection::TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::InetAddress> localAddr, std::shared_ptr<net::Channel> channel, AcceptHandler *handler)
    {
        this->remote_addr_ = remoteAddr;
        this->local_addr_ = localAddr;
        this->channel_ = channel;

        this->channel_->set_handler(this);
        this->handler_ = handler;

        input_buffer_ = std::make_shared<Buffer>();
        output_buffer_ = std::make_shared<Buffer>();

        closed = false;
        
        file_.open("/home/yuan/Videos/The.Shawshank.Redemption.1994.1080p.BluRay.H264.AAC-RARBG.mp4");
        if (!file_.good()) {
            if (handler_) {
                handler_->on_close(this);
                std::cout << "cant open file!!!\n";
            }
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            if (handler_) {
                handler_->on_close(this);
                std::cout << "cant open file!!!\n";
            }
        }

        file_.clear();
        file_.seekg(0, std::ios::beg);

        buff1_ = new char[1024 * 1024];
    }

    TcpConnection::~TcpConnection()
    {
        std::cout << "free =====> !!!\n";
    }

    bool TcpConnection::is_connected()
    {
        return closed;
    }

    const InetAddress & TcpConnection::get_remote_address() const
    {
        return *remote_addr_.get();
    }

    const InetAddress & TcpConnection::get_local_address() const
    {
        return *local_addr_.get();
    }

    void TcpConnection::send(Buffer *buff)
    {
        int fd = this->channel_->get_fd();
        ::send(fd, buff->begin(), buff->remain_bytes(), 0);
    }

    // 丢弃所有未发送的数据
    void TcpConnection::abort()
    {
        closed = true;
    }

    // 发送完数据后返回
    void TcpConnection::close()
    {
        closed = true;
    }

    ConnectionType TcpConnection::get_conn_type()
    {
        return ConnectionType::TCP;
    }

    Channel * TcpConnection::get_channel()
    {
        return channel_.get();
    }

    void TcpConnection::on_read_event()
    {
        input_buffer_->rewind();

        // TODO read data
        int fd = channel_->get_fd();
        while (true) {
            int bytes = recv(fd, input_buffer_->begin(), input_buffer_->writable_size(), 0);
            if (bytes <= 0) {
                if (bytes == 0) {
                    if (handler_) {
                        handler_->on_close(this);
                        return;
                    } else {
                        // TODO error log
                    }
                } else if (bytes < 0) {
                    std::cout << "xxxxxxxxxxxxxxxxxxxxxxxxxxx22222222\n";
                    break;
                }

                std::cout << "xxxxxxxxxxxxxxxxxxxxxxxxxxx\n";
                // close
                break;
            }
            
            input_buffer_->fill(bytes);
            input_buffer_->resize();

            break;
        }

        if (!req_.parse_header(*input_buffer_.get())) {
            std::cout << "parse fail!!" << std::endl;
            if (handler_) {
                handler_->on_close(this);
                std::cout << "parse http request fail, close connection now\n";
            }
        } else {
            if (req_.get_url_domain().size() > 1 && req_.get_url_domain()[1] == "movie") {
                

                const std::string *range = req_.get_header(net::http::http_header_key::range);
                long long offset = 0;

                if (range) {
                    size_t pos = range->find_first_of("=");
                    if (std::string::npos == pos) {
                        handler_->on_close(this);
                        return;
                    }

                    size_t pos1 = range->find_first_of("-");
                    offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
                }
                
                file_.seekg(offset, std::ios::beg);
                file_.read(buff1_, 1024 * 1024);
                size_t r = file_.gcount();

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
                    std::cout << "offset: " << offset << ", length: " << r << ", size: " << length_  << ", fd:" << fd << std::endl;
                    write(fd, resp.c_str(), resp.size());
                    write(fd, buff1_, r);

                    if (file_.eof()) {
                        file_.clear();
                    }

                    file_.seekg(0, std::ios::beg);
                } else {
                    std::string resp = "HTTP/1.1 206 Partial Content\r\n";
                    resp.append("Content-Type: video/mp4\r\n");
                    resp.append("Accept-Ranges: bytes\r\n");
                    resp.append("Content-Range: bytes ")
                        .append(std::to_string(offset))
                        .append("-")
                        .append(std::to_string(length_ - 1))
                        .append("/")
                        .append(std::to_string(length_))
                        .append("\r\n");
                    
                    resp.append("Content-length: ").append(std::to_string(r)).append("\r\n\r\n");
                    //resp.append("Content-Disposition: attachment; filename=The.Shawshank.Redemption.1994.1080p.BluRay.H264.AAC-RARBG.mp4\r\n");
                    write(fd, resp.c_str(), resp.size());
                    write(fd, buff1_, r);

                    file_.seekg(0, std::ios::beg);
                }

                req_.reset();
                return;
            }

            std::cout << "content length:" << req_.header_exists(net::http::http_header_key::content_length) << std::endl;
            std::string msg = "你好，世界！！";
            std::string repsonse = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
            write(fd, repsonse.c_str(), repsonse.size());
            handler_->on_close(this);
        }
    }

    void TcpConnection::on_write_event()
    {
        // TODO write data

    }

    int TcpConnection::get_fd()
    {
        return 0;
    }
}