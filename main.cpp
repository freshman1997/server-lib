#include "net/base/acceptor/udp_acceptor.h"
#include "net/base/poller/select_poller.h"
#include "net/dns/dns_server.h"
#include "net/ftp/server/ftp_server.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <ctime>
#include <cstdlib>

#ifndef _WIN32
#include <signal.h>
#else
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#endif

#include "base/utils/compressed_trie.h"
#include "net/base/acceptor/acceptor.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/http/http_server.h"
#include "net/base/poller/poller.h"
#include "thread/runnable.h"
#include "thread/thread_pool.h"
#include "net/base/socket/socket.h"

#include "timer/timer_task.h"
#include "timer/timer.h"
#include "timer/wheel_timer_manager.h"

#include "net/base/event/event_loop.h"
#include "net/base/connection/connection.h"

class PrintTask : public thread::Runnable
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
};

void test_evloop()
{
    net::Socket *sock = new net::Socket("127.0.0.1", 12333);
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

    net::Poller *poller = new net::SelectPoller;
    timer::WheelTimerManager *manager = new timer::WheelTimerManager;
    TimerTask *t = new PrintTask1;
    Timer *timer = manager->interval(2000, 2000, t, 100);

    net::EventLoop loop(poller, manager);
    acceptor->set_event_handler(&loop);
    loop.loop();
}

void test_url()
{
    base::CompressTrie trie;
    trie.insert("/.cache/clangd/");
    trie.insert("/include/base");

    trie.insert("/static", true);

    if (trie.find_prefix("/static/cz", true) < 0) {
        std::cout << "start with\n";
    }
}

void test_udp()
{
    net::Socket *sock = new net::Socket("127.0.0.1", 12333, true);
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

    timer::WheelTimerManager manager;
    net::Acceptor *acceptor = new net::UdpAcceptor(sock, &manager);
    if (!acceptor->listen()) {
        std::cout << " listen failed " << std::endl;
        return;
    }

    net::SelectPoller poller;
    net::EventLoop loop(&poller, &manager);
    loop.loop();
}

void sigpipe_handler(int signum) 
{
    std::cout << "Caught SIGPIPE signal: " << signum << '\n';
}

#ifdef _WIN32
extern std::string UTF8ToGBEx(const char *utf8);
#endif

void test_args()
{
    cout << '\n';
}

template <typename T, typename ...Args>
void test_args(T arg, Args ...args)
{
    cout << arg << ", ";
    if (sizeof ...(args) > 0) {
        test_args(args...);
    }
}

int main()
{
    test_args(1, 2, 3, 4, 5);
#ifndef _WIN32
    // 注册SIGPIPE信号处理函数
    signal(SIGPIPE, sigpipe_handler);
#else
    WSADATA wsa;
    if (const int iResult = WSAStartup(MAKEWORD(2, 2), &wsa);iResult != NO_ERROR) {
        wprintf(L"WSAStartup failed with error: %d\n", iResult);
        return 1;
    }
#endif
    
    //test_http_client();
    //return 0;
    srand(time(nullptr));

    /*std::fstream file(UTF8ToGBEx("E:/迅雷下载/EE7E7F2648291605782E3CB59033DE7BED4A9E65.torrent"), std::ios_base::in);
    if (file.good()) {
        file.seekg(0, std::ios_base::end);
        std::size_t len = file.tellg();
        file.seekg(0, std::ios_base::beg);
        Buffer buf(len);
        file.read(buf.buffer_begin(), len);
        buf.fill(len);

        const auto &data = net::bit_torrent::BencodingDataConverter::parse(buf.peek(), buf.peek() + buf.readable_bytes());
        if (data) {
            std:: cout << data->to_string() << std::endl;
        } else {
            std:: cout << "parse failed!" << std::endl;
        }
    }*/

    net::dns::DnsServer server;
    server.serve(9090);

    //std::cout << base::util::base64_encode("hello:hello1") << std::endl;
    //std::cout << base::util::base64_decode("aGVsbG86aGVsbG8x") << std::endl;
#ifdef _WIN32
    WSACleanup();
#endif
    //thread::ThreadPool pool;
    //pool.push_task(new PrintTask);
    //pool.push_task(new PrintTask);
    //pool.start();

    return 0;
}