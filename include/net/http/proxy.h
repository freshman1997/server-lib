#ifndef __NET_HTTP_PROXY_H__
#define __NET_HTTP_PROXY_H__
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/utils/compressed_trie.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/inet_address.h"
#include "net/http/common.h"
#include "timer/timer_task.h"

namespace net::http 
{
    class HttpServer;
    class RemoteConnectTask;
    class HttpProxy;

    class RemoteConnectTask : public timer::TimerTask
    {
    public:
        RemoteConnectTask(int id, HttpRequest *req, HttpResponse *resp, const InetAddress &addr
            , HttpProxy *proxy, const std::string &url, const std::string &client_addr) 
            : task_id_(id), req_(req), resp_(resp), addr_(addr), proxy_(proxy), url_(url), client_addr_(client_addr)
        {}

    public:
        virtual void on_timer(timer::Timer *timer);

        virtual void on_finished(timer::Timer *timer);

    public:
        int task_id_;
        HttpRequest  *req_ = nullptr;
        HttpResponse *resp_ = nullptr;
        const InetAddress addr_;
        HttpProxy *proxy_ = nullptr;
        std::string url_;
        std::string client_addr_;
    };

    class HttpProxy : public ConnectionHandler
    {
    public:
        HttpProxy();
        HttpProxy(HttpServer *server);
        ~HttpProxy();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool load_proxy_config_and_init();

        void set_server(HttpServer *server)
        {
            server_ = server;
        }

        bool is_proxy(const std::string &url) const;

        void on_client_close(Connection *conn);

        void serve_proxy(HttpRequest *req, HttpResponse *resp);

        void on_connection_timeout(RemoteConnectTask *task);

    private:
        bool init_proxy_connection(const std::string &ip, short port, int taskId);

        void put_conncetion(Connection *conn);

        int gen_task_id();

        void do_forward_packet(HttpResponse *resp, Connection *conn, Buffer *buf1, Buffer *buf2);

    private:
        int tasK_id_;

        // <url, <ip, port>>
        std::unordered_map<std::string, std::vector<InetAddress>> proxy_configs_;

        // <url, [conn, ...]>
        std::unordered_map<std::string, std::set<Connection *>> url_connection_mapping_;

        // <server conn, <url, client conn>>
        std::unordered_map<Connection *, std::pair<std::string, Connection *>> sc_connection_mapping_;

        // <client conn, server conn>
        std::unordered_map<Connection *, Connection *> cs_connection_mapping_;

        // server instance
        HttpServer *server_;

        // for url
        base::CompressTrie url_trie_;

        // <taskId, conn timer>
        std::unordered_map<uint32_t, timer::Timer *> conn_tasks_;

        // <client addr, taskId>
        std::unordered_map<std::string, uint32_t> task_client_mapping_;
    };
}

#endif