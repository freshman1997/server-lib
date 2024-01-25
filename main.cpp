#include "buffer/buffer.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/http/header_key.h"
#include "net/http/request_context.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <fstream>

#include "net/poller/epoll_poller.h"
#include "net/poller/poller.h"
#include "thread/task.h"
#include "thread/thread_pool.h"
#include "net/socket/socket.h"

#include "timer/timer_task.h"
#include "timer/timer.h"
#include "timer/wheel_timer_manager.h"

#include "net/event/event_loop.h"
#include "net/connection/connection.h"


class PrintTask : public thread::Task
{
protected:

    virtual void run_internal()
    {
        std::cout << "printing...\n";
    }
};

using namespace std;
using namespace timer;
class PrintTask1 : public TimerTask
{
    int idx = 0;
public:
    virtual void on_timer(Timer *timer)
    {
        std::cout << " timer task printing ==> " << idx <<  std::endl;
        ++idx;
    }

    virtual void on_finished(Timer *timer)
    {
        std::cout << " timer task finished " << std::endl;
    }
};

class VideoTest
{
public:
    VideoTest()
    {
        file_.open("/home/yuan/Videos/The.Shawshank.Redemption.1994.1080p.BluRay.H264.AAC-RARBG.mp4");
        if (!file_.good()) {
            std::cout << "open file fail!\n";
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            std::cout << "open file fail1!\n";
        }

        file_.seekg(0, std::ios::beg);
        buff = std::make_shared<Buffer>(1024 * 1024 * 2);
    }

    void on_request(std::shared_ptr<net::http::HttpRequestContext> context)
    {
        buff->reset();
        net::http::HttpRequest req_ = *context->get_request();
        if (req_.get_url_domain().size() > 1 && req_.get_url_domain()[1] == "movie") {
            const std::string *range = req_.get_header(net::http::http_header_key::range);
            long long offset = 0;

            if (range) {
                size_t pos = range->find_first_of("=");
                if (std::string::npos == pos) {
                    context->get_connection()->close();
                    return;
                }

                size_t pos1 = range->find_first_of("-");
                offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
            }
            
            file_.seekg(offset, std::ios::beg);
            file_.read(buff->buffer_begin(), 1024 * 1024 * 2);
            size_t r = file_.gcount();
            buff->fill(r);

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

                std::shared_ptr<Buffer> buff1 = std::make_shared<Buffer>();
                buff1->write_string(resp);
                context->get_connection()->send(buff1);

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
                std::shared_ptr<Buffer> buff1 = std::make_shared<Buffer>();
                buff1->write_string(resp);
                context->get_connection()->send(buff1);
            }

            context->get_connection()->send(buff);

            if (file_.eof()) {
                file_.clear();
            }

            file_.seekg(0, std::ios::beg);
            return;
        }

        std::cout << "content length:" << req_.header_exists(net::http::http_header_key::content_length) << std::endl;
        std::string msg = "你好，世界！！";
        std::string repsonse = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
        std::shared_ptr<Buffer> buff1 = std::make_shared<Buffer>();
        buff1->write_string(repsonse);
        context->get_connection()->send(buff1);
    }

private:
    std::fstream file_;
    long long length_;
    std::shared_ptr<Buffer> buff;
};

void test_evloop()
{
    net::Socket *sock = new net::Socket("", 12333);
    if (!sock->valid()) {
        cout << "create socket fail!!\n";
        return;
    }

    sock->set_reuse(true);

    if (!sock->bind()) {
        std::cout << " bind failed " << std::endl;
        return;
    }

    //sock->set_none_block(true);
    net::Acceptor *acceptor = new net::TcpAcceptor(sock);
    if (!acceptor->listen()) {
        std::cout << " listen failed " << std::endl;
        return;
    }

    net::Poller *poller = new net::EpollPoller;
    timer::WheelTimerManager *manager = new timer::WheelTimerManager;
    TimerTask *t = new PrintTask1;
    Timer *timer = manager->interval(2000, 2000, t, 100);

    net::EventLoop loop(poller, manager, acceptor);
    acceptor->set_handler(&loop);
    loop.loop();
}

void test_http_server()
{
    net::http::HttpServer server;
    VideoTest vt;

    server.on("/movie", [&vt](std::shared_ptr<net::http::HttpRequestContext> ctx) {
        vt.on_request(ctx);
    });

    if (!server.init(12333)) {
        std::cout << " init failed " << std::endl;
        return;
    }

    server.serve();
}

int main()
{
    test_http_server();
    return 0;
}