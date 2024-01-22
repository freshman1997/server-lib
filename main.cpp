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
    
    /*
    thread::ThreadPool pool(1);

    pool.start();

    PrintTask *task = new PrintTask;
    pool.push_task(task);
    */

    using namespace nlohmann;
    json data = {
        {"name", "tomcat"}
    };

    cout << data << endl;

    return 0;
}