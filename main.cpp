#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <memory>
#include <string>
#include <fstream>

#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/http/header_key.h"
#include "net/http/request_context.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
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

#ifdef _WIN32
#pragma comment(lib,"ws2_32.lib")
#endif

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
        file_.open("/home/yuan/Desktop/cz");
        if (!file_.good()) {
            std::cout << "open file fail!\n";
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            std::cout << "open file fail1!\n";
        }
    }

    void on_request(std::shared_ptr<net::http::HttpRequestContext> context)
    {
        auto out = context->get_connection()->get_output_buff();
        if (out->get_buff_size() < 1024 * 10224 * 2) {
            out->resize(1024 * 10224 * 2);
        }

        net::http::HttpRequest request = *context->get_request();
        const std::string *range = request.get_header(net::http::http_header_key::range);
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

        std::string resp = "HTTP/1.1 206 Partial Content\r\nContent-Type: video/mp4\r\n";
        resp.append("Content-Range: bytes ")
            .append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length_ - 1))
            .append("/")
            .append(std::to_string(length_))
            .append("\r\n");
        
        size_t r = 1024 * 1024 * 2 + offset > length_ ? length_ - offset : 1024 * 1024 * 2;
        std::cout << "offset: " << offset << ", length: " << r << ", size: " << length_ << std::endl;

        resp.append("Content-length: ").append(std::to_string(r)).append("\r\n\r\n");
        out->write_string(resp);

        file_.seekg(offset, std::ios::beg);
        file_.read(out->buffer_begin(), 1024 * 1024 * 2);

        out->fill(r);
        context->get_connection()->send();

        if (file_.eof()) {
            file_.clear();
        }
    }

private:
    std::fstream file_;
    long long length_;
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
    acceptor->set_event_handler(&loop);
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