#ifndef __NET_HTTP_PROXY_H__
#define __NET_HTTP_PROXY_H__
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "base/utils/compressed_trie.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/inet_address.h"
#include "net/http/common.h"

namespace net::http 
{
    class HttpServer;

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

    private:
        bool init_proxy_connection(const std::pair<std::string, short> &cfg);

        void put_conncetion(Connection *conn);

    private:
        // <url, <ip, port>>
        std::unordered_map<std::string, InetAddress> proxy_configs_;

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
    };
}

#endif