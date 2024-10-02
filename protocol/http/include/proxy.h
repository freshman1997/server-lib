#ifndef __NET_HTTP_PROXY_H__
#define __NET_HTTP_PROXY_H__
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/utils/compressed_trie.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "common.h"
#include "timer/timer_task.h"

namespace net::http 
{
    class HttpServer;
    class RemoteConnectTask;
    class HttpProxy;

    class RemoteConnectTask : public timer::TimerTask
    {
    public:
        RemoteConnectTask(Connection *conn, HttpRequest *req, HttpResponse *resp, Connection *remoteConn,
            HttpProxy *proxy, const std::string &url, const std::string &client_addr) 
            : conn_(conn), req_(req), resp_(resp), remote_conn_(remoteConn), proxy_(proxy), url_(url), client_addr_(client_addr)
        {}

    public:
        virtual void on_timer(timer::Timer *timer);

    public:
        Connection *conn_ = nullptr;
        HttpRequest  *req_ = nullptr;
        HttpResponse *resp_ = nullptr;
        Connection *remote_conn_;
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

        void check_response_timer(Connection *conn);

    private:
        Connection * init_proxy_connection(const std::string &ip, short port);

        void put_conncetion(Connection *conn);

        void do_forward_packet(HttpResponse *resp, Connection *conn, Buffer *buf1, Buffer *buf2);

        void clear_connection_pending_request(Connection *conn);

    private:
        // <url, <ip, port>>
        std::unordered_map<std::string, std::vector<InetAddress>> proxy_configs_;

        // <server conn, <url, client conn>>
        std::unordered_map<Connection *, Connection *> sc_connection_mapping_;

        // <client conn, server conn>
        std::unordered_map<Connection *, Connection *> cs_connection_mapping_;

        // server instance
        HttpServer *server_;

        // for url
        base::CompressTrie url_trie_;

        // <taskId, conn timer>
        std::unordered_map<Connection *, timer::Timer *> conn_tasks_;

        // pending requests after the connect task
        std::unordered_map<Connection *, std::vector<Buffer *>> pending_requests_;
    };
}

#endif