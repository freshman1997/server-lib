#include "buffer/buffer.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/http/header_key.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <fstream>

#include "net/poller/epoll_poller.h"
#include "net/poller/poller.h"
#include "net/socket/inet_address.h"
#include "net/socket/tcp_socket.h"
#include "nlohmann/json_fwd.hpp"
#include "nlohmann/json.hpp"
#include "thread/task.h"
#include "thread/thread_pool.h"
#include "net/socket/socket.h"

#include "timer/timer_task.h"
#include "timer/timer.h"
#include "timer/wheel_timer_manager.h"

#include "net/event/event_loop.h"



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
    if (!server.init(12333)) {
        std::cout << " init failed " << std::endl;
        return;
    }

    server.serve();
}

int main()
{
    test_http_server();
    return 1;

    /*TimerTask *t = new PrintTask1;
    WheelTimerManager *manager = new WheelTimerManager;
    Timer *timer = manager->timeout(2000, t);
    manager->tick();
    timer->reset();
    manager->interval(2000, 2000, t, 100);
    while (true)
    {
        manager->tick();

        std::this_thread::sleep_for(chrono::milliseconds(10));
    }

    return 0;*/
    using namespace net;

    Socket sock("192.168.88.50", 12334);
    if (!sock.valid()) {
        cout << "create socket fail!!\n";
        return 1;
    }

    sock.set_reuse(true);
    if (!sock.bind()) {
        cout << "cant bind!!\n";
        return 1;
    }

    if (!sock.listen()) {
        cout << "cant listen!!\n";
        return 1;
    }

    struct sockaddr_in peer_addr;
    int conn_fd = sock.accept(peer_addr);
    if (conn_fd < 0) {
        cout << "cant accept!!\n";
        return 1;
    }

    /*
    thread::ThreadPool pool(1);

    pool.start();

    PrintTask *task = new PrintTask;
    pool.push_task(task);
    */

    Buffer buff;
    int time = 0;

    fstream file;
    file.open("/home/yuan/Videos/The.Shawshank.Redemption.1994.1080p.BluRay.H264.AAC-RARBG.mp4");
    if (!file.good()) {
        return 1;
    }

    file.seekg(0, std::ios_base::end);
    long long length = file.tellg();
    if (length == 0) {
        file.close();
        return 1;
    }

    file.clear();
    file.seekg(0, ios::beg);

    char *buff1 = new char[1024 * 1024];
    net::http::HttpRequest req;

    while (true) {
        buff.rewind();
        int ret = recv(conn_fd, buff.begin(), 1024, 0);
        if (ret == 0 || ret < 0) {
            break;
        }

        buff.set_write_index(ret);
        req.reset();

        if (!req.parse_header(buff)) {
            std::cout << "parse fail!!" << std::endl;
            continue;
        } else {
            if (req.get_url_domain().size() > 1 && req.get_url_domain()[1] == "movie") {
                if (file.bad()) {
                    cout << "file desciptor error!!\n";
                    break;
                }

                string resp = "HTTP/1.1 206\r\n";
                resp.append("Content-Type: video/mp4\r\n");

                const string *range = req.get_header(net::http::http_header_key::range);
                long long offset = 0;

                if (range) {
                    size_t pos = range->find_first_of("=");
                    if (string::npos == pos) {
                        close(conn_fd);
                        file.close();
                        return 1;
                    }

                    size_t pos1 = range->find_first_of("-");
                    offset = std::atol(range->substr(pos + 1, pos1 - pos).c_str());
                }
                
                cout.flush();
                
                
                file.seekg(offset, ios::beg);
                file.read(buff1, 1024 * 1024);
                size_t r = file.gcount();

                resp.append("Content-Range: bytes ")
                    .append(std::to_string(offset))
                    .append("-")
                    .append(std::to_string(length - 1))
                    .append("/")
                    .append(std::to_string(length))
                    .append("\r\n");

                resp.append("Content-length: ").append(std::to_string(r)).append("\r\n\r\n");
                cout << "offset: " << offset << ", length: " << r << ", size: " << length << endl;
                write(conn_fd, resp.c_str(), resp.size());
                write(conn_fd, buff1, r);

                if (file.eof()) {
                    file.clear();
                }

                file.seekg(0, ios::beg);
                continue;
            }

            std::cout << "content lenth:" << req.header_exists(net::http::http_header_key::content_length) << std::endl;
            string msg = "你好，世界！！";
            string repsonse = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
            write(conn_fd, repsonse.c_str(), repsonse.size());
            close(conn_fd);
            break;
        }
    }

    file.close();
    string raw = "GET /Michel4Liu/article/details/79531484 HTTP/1.1\r\n"
                "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
                "Accept-Encoding: gzip, deflate, br\r\n"
                "Accept-Language: zh-CN,zh;q=0.9,en;q=0.8,en-GB;q=0.7,en-US;q=0.6,ja;q=0.5\r\n"
                "Cache-Control: max-age=0\r\n"
                "Connection: keep-alive\r\n"
                "Cookie: BAIDU_SSP_lcr=https://cn.bing.com/; uuid_tt_dd=10_19001793670-1694633276924-244360; UserName=qq_38214064; UserInfo=f8a19b802ec945c68e80b86d51e46c94; UserToken=f8a19b802ec945c68e80b86d51e46c94; UserNick=1997HelloWorld; AU=9E6; UN=qq_38214064; BT=1697109839848; p_uid=U010000; Hm_up_6bcd52f51e9b3dce32bec4a3997715ac=%7B%22islogin%22%3A%7B%22value%22%3A%221%22%2C%22scope%22%3A1%7D%2C%22isonline%22%3A%7B%22value%22%3A%221%22%2C%22scope%22%3A1%7D%2C%22isvip%22%3A%7B%22value%22%3A%220%22%2C%22scope%22%3A1%7D%2C%22uid_%22%3A%7B%22value%22%3A%22qq_38214064%22%2C%22scope%22%3A1%7D%7D; FCNEC=%5B%5B%22AKsRol_dIVf4-VQOY7CiFm34ECeBp1GozYTEzQhNu6ffblRDN2Qmts-21bKguzoZmCSEMwVhvAWpVieYcHPqwCGnWicmHg4C74ICHSYfhAXGZFOgwzJOzhK6MIoKC9eAcQ_sdhSSzhG33fGdXLO10Niow2M_R8nUYg%3D%3D%22%5D%2Cnull%2C%5B%5D%5D; qq_38214064comment_new=1668302555115; c_pref=; c_ref=https%3A//cn.bing.com/; c_first_ref=cn.bing.com; c_segment=8; firstDie=1; Hm_lvt_6bcd52f51e9b3dce32bec4a3997715ac=1700397786,1700666320,1700750164,1700982523; dc_sid=50462ddeca8d7b1bcd832d32f785ff84; creative_btn_mp=3; log_Id_click=194; dc_session_id=10_1701003122663.383229; SidecHatdocDescBoxNum=true; c_first_page=https%3A//blog.csdn.net/Michel4Liu/article/details/79531484; c_dsid=11_1701003606400.233306; c_page_id=default; log_Id_pv=197; Hm_lpvt_6bcd52f51e9b3dce32bec4a3997715ac=1701003607; __gads=ID=a6bc64bbcb8cc2bf-225e355da0e300c7:T=1694633278:RT=1701003607:S=ALNI_MZbnafmwW1OKSyCTUvApRUbzlROWQ; __gpi=UID=00000d926eba86f6:T=1694633278:RT=1701003607:S=ALNI_MZEm9oG8xzsvLNJGJkdu7tdBy1NRw; log_Id_view=5691; dc_tos=s4qfyh\r\n"
                "Host: blog.csdn.net\r\n"
                "Referer: https://cn.bing.com/\r\n"
                "Sec-Fetch-Dest: document\r\n"
                "Sec-Fetch-Mode: navigate\r\n"
                "Sec-Fetch-Site: cross-site\r\n"
                "Sec-Fetch-User: ?1\r\n"
                "Upgrade-Insecure-Requests: 1\r\n"
                "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36 Edg/119.0.0.0\r\n"
                "sec-ch-ua: \"Microsoft Edge\";v=\"119\", \"Chromium\";v=\"119\", \"Not?A_Brand\";v=\"24\"\r\n"
                "sec-ch-ua-mobile: ?0\r\n"
                "sec-ch-ua-platform: \"Windows\"\r\n\r\n";

    //buff.write_string(raw);
    

    using namespace nlohmann;
    json data = {
        {"name", "tomcat"}
    };

    cout << data << endl;

    return 0;
}