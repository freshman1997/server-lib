#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <memory>
#include <string>
#include <fstream>
#include <ctime>
#include <cstdlib>

#include "buffer/buffer.h"
#include "net/base/acceptor/acceptor.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/bit_torrent/structure/bencoding.h"
#include "net/http/header_key.h"
#include "net/http/request_context.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/base/poller/epoll_poller.h"
#include "net/base/poller/poller.h"
#include "net/base/poller/select_poller.h"
#include "nlohmann/json_fwd.hpp"
#include "thread/task.h"
#include "thread/thread_pool.h"
#include "net/base/socket/socket.h"

#include "timer/timer_task.h"
#include "timer/timer.h"
#include "timer/wheel_timer_manager.h"

#include "net/base/event/event_loop.h"
#include "net/base/connection/connection.h"

#include "base/utils/base64.h"
#include "nlohmann/json.hpp"
#include "net/http/content/parsers/multipart_form_data_parser.h"

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
        content_size_ = -1;
        file_.open("/home/yuan/Desktop/cz");
        if (!file_.good()) {
            std::cout << "open file fail!\n";
            return;
        }

        file_.seekg(0, std::ios_base::end);
        length_ = file_.tellg();
        if (length_ == 0) {
            file_.close();
            std::cout << "open file fail1!\n";
            return;
        }

        content_size_ = 1024 * 1024;
    }

    void on_request(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        if (content_size_ < 0) {
            resp->get_context()->process_error();
            return;
        }

        const std::string *range = req->get_header(net::http::http_header_key::range);
        long long offset = 0;

        if (range) {
            size_t pos = range->find_first_of("=");
            if (std::string::npos == pos) {
                resp->get_context()->process_error();
                return;
            }

            size_t pos1 = range->find_first_of("-");
            offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
        }

        std::string bytes = "bytes ";
        bytes.append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length_ - 1))
            .append("/")
            .append(std::to_string(length_));
        
        resp->add_header("Content-Type", "video/mp4");
        resp->add_header("Content-Range", bytes);

        size_t r = content_size_ + offset > length_ ? length_ - offset : content_size_;
        //std::cout << "offset: " << offset << ", length: " << r << ", size: " << length_ << std::endl;

        resp->add_header("Content-length", std::to_string(r));

        file_.seekg(offset, std::ios::beg);
        resp->get_buff()->reset();
        if (resp->get_buff()->writable_size() < content_size_) {
            resp->get_buff()->resize(content_size_);
        }

        file_.read(resp->get_buff()->buffer_begin(), content_size_);
        resp->get_buff()->fill(r);

        if (file_.eof()) {
            file_.clear();
        }

        resp->add_header("Accept-Ranges", "bytes");
        resp->set_response_code(net::http::ResponseCode::partial_content);
        resp->send();
    }

    void on_body_test(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        if (req->body_begin()) {
            std::cout << "has body\n";
        }

        /*using namespace nlohmann;
        json jData = json::parse(req->body_begin(), req->body_end());
        */
        std::string jData(req->body_begin(), req->body_end());
        req->read_body_done();

        std::cout << jData << std::endl;

        std::string body = "{\"success\": 1}";
        resp->add_header("Content-length", std::to_string(body.size()));
        resp->add_header("Connection", "close");
        resp->add_header("Content-Type", "application/json");
        resp->set_response_code(net::http::ResponseCode::ok_);

        resp->append_body(body);
        resp->send();
    }

    void icon(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {
        resp->add_header("Connection", "close");
        resp->add_header("Content-Type", "image/x-icon");
        resp->set_response_code(net::http::ResponseCode::ok_);
        std::fstream file;
        file.open("/home/yuan/Desktop/icon.ico");
        if (!file.good()) {
            resp->get_context()->process_error();
            return;
        }

        file.seekg(0, std::ios_base::end);
        std::size_t sz = file.tellg();
        resp->get_buff()->resize(sz);
        file.seekg(0, std::ios_base::beg);
        file.read(resp->get_buff()->buffer_begin(), sz);
        resp->get_buff()->fill(sz);

        resp->add_header("Content-length", std::to_string(sz));
        resp->send();
    }

    void serve_static(net::http::HttpRequest *req, net::http::HttpResponse *resp)
    {

    }

private:
    int content_size_;
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
    sock->set_none_block(true);
    if (!sock->bind()) {
        std::cout << " bind failed " << std::endl;
        return;
    }

    net::Acceptor *acceptor = new net::TcpAcceptor(sock);
    if (!acceptor->listen()) {
        std::cout << " listen failed " << std::endl;
        return;
    }

    net::Poller *poller = new net::EpollPoller;
    timer::WheelTimerManager *manager = new timer::WheelTimerManager;
    TimerTask *t = new PrintTask1;
    Timer *timer = manager->interval(2000, 2000, t, 100);

    net::EventLoop loop(poller, manager);
    acceptor->set_event_handler(&loop);
    loop.loop();
}

void test_http_server()
{
    net::http::HttpServer server;
    VideoTest vt;

    server.on("/movie", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.on_request(req, resp);
    });

    server.on("/body", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.on_body_test(req, resp);
    });

    server.on("/favicon.ico", [&vt](net::http::HttpRequest *req, net::http::HttpResponse *resp) {
        vt.icon(req, resp);
    });

    if (!server.init(12333)) {
        std::cout << " init failed " << std::endl;
        return;
    }

    server.serve();
}

int main()
{
    srand(time(nullptr));

    std::fstream file("/home/yuan/Desktop/1.torrent", std::ios_base::in);
    if (file.good()) {
        file.seekg(0, std::ios_base::end);
        std::size_t len = file.tellg();
        file.seekg(0, std::ios_base::beg);
        Buffer buf(len);
        file.read(buf.buffer_begin(), len);
        buf.fill(len);

        const auto &data = net::bit_torrent::BencodingDataConverter::parse(buf.peek(), buf.peek() + buf.readable_bytes());
        if (!data) {
            std:: cout << data->to_string() << std::endl;
        } else {
            std:: cout << "parse failed!" << std::endl;
        }
    }

    test_http_server();
    //std::cout << base::util::base64_encode("hello:hello1") << std::endl;
    //std::cout << base::util::base64_decode("aGVsbG86aGVsbG8x") << std::endl;

    return 0;
}