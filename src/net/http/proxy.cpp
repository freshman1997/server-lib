#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iterator>

#include "net/http/proxy.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/socket/socket.h"
#include "net/http/ops/config_manager.h"
#include "net/http/response_code.h"
#include "singleton/singleton.h"
#include "nlohmann/json.hpp"
#include "net/http/context.h"
#include "net/http/request.h"
#include "net/http/response.h"
#include "net/http/http_server.h"
#include "net/base/event/event_loop.h"

namespace net::http
{
    HttpProxy::HttpProxy()
    {
        server_ = nullptr;
        srand(time(nullptr));
    }

    HttpProxy::HttpProxy(HttpServer *server) : server_(server)
    {
        srand(time(nullptr));
    }

    HttpProxy::~HttpProxy()
    {
        for (const auto &it : url_connection_mapping_) {
            for (const auto &conn : it.second) {
                conn->close();
            }
        }

        url_connection_mapping_.clear();
        cs_connection_mapping_.clear();
        sc_connection_mapping_.clear();
    }

    void HttpProxy::on_connected(Connection *conn)
    {
        
    }

    void HttpProxy::on_error(Connection *conn)
    {

    }

    void HttpProxy::on_read(Connection *conn)
    {
        auto it = sc_connection_mapping_.find(conn);
        if (it == sc_connection_mapping_.end()) {
            std::cout << "client connection closed\n";
            return;
        }

        // forwarding
        it->second.second->send(conn->get_input_buff(true));
    }

    void HttpProxy::on_write(Connection *conn)
    {
        auto it = sc_connection_mapping_.find(conn);
        if (it == sc_connection_mapping_.end()) {
            put_conncetion(conn);
            std::cout << "connect to server, ip: " << conn->get_remote_address().get_ip() << ", port: " << conn->get_remote_address().get_port() << " successfully\n";
        }
    }

    void HttpProxy::on_close(Connection *conn)
    {
        auto it = sc_connection_mapping_.find(conn);
        if (it != sc_connection_mapping_.end()) {
            if (it->second.second) {
                cs_connection_mapping_.erase(it->second.second);
            }
            url_connection_mapping_[it->second.first].erase(conn);
            sc_connection_mapping_.erase(it);
        }
    }

    bool HttpProxy::init_proxy_connection(const std::pair<std::string, short> &cfg)
    {
        net::Socket *sock = new net::Socket(cfg.first.c_str(), cfg.second);
        if (!sock->valid()) {
            std::cout << "create socket fail, ip: " << cfg.first << ", port: " << cfg.second << std::endl;
            return false;
        }

        if (!sock->connect()) {
            std::cout << " connect failed, ip: " << cfg.first << ", port: " << cfg.second << std::endl;
            return false;
        }

        Connection *conn = new TcpConnection(sock);
        conn->set_connection_handler(this);
        conn->set_event_handler(server_->get_event_loop());
        server_->get_event_loop()->update_event(conn->get_channel());
        
        server_->get_event_loop()->on_new_connection(conn, false);

        return true;
    }

    bool HttpProxy::load_proxy_config_and_init()
    {
        auto &cfgManager = singleton::Singleton<HttpConfigManager>();
        if (!cfgManager.good()) {
            return false;
        }

        const auto &proxiesCfg = cfgManager.get_type_array_properties<nlohmann::json>("proxies");
        if (proxiesCfg.empty()) {
            return false;
        }

        assert(server_);

        std::cout << "loading proxy configs...\n";
        for (const auto &proxyCfg : proxiesCfg) {
            if (!proxyCfg.is_object()) {
                continue;
            }

            const auto &target = proxyCfg["target"];
            if (!target.is_array() || target.size() < 2 || !target[0].is_string() || !target[1].is_number_unsigned()) {
                continue;
            }

            const std::string &root = proxyCfg["root"];
            const std::string &ip = target[0];
            short port = target[1];
            proxy_configs_[proxyCfg["root"]] = {ip, port};

            url_trie_.insert(root, true);

            if (!init_proxy_connection({ip, port})) {
                return false;
            }
        }

        std::cout << "proxy configs loaded: " << proxy_configs_.size() << " item\n";

        return true;
    }

    bool HttpProxy::is_proxy(const std::string &url) const
    {
        return url_trie_.find_prefix(url, true) < 0;
    }

    void HttpProxy::on_client_close(Connection *conn)
    {
        auto it = cs_connection_mapping_.find(conn);
        if (it != cs_connection_mapping_.end()) {
            if (it->second) {
                sc_connection_mapping_.erase(it->second);
            }
            cs_connection_mapping_.erase(it);
        }
    }

    void HttpProxy::serve_proxy(HttpRequest *req, HttpResponse *resp)
    {
        Connection *conn = req->get_context()->get_connection();
        auto it = cs_connection_mapping_.find(conn);
        Connection *useConn = nullptr;

        if (it != cs_connection_mapping_.end()) {
            useConn = it->second;
        } else {
            int idx = -url_trie_.find_prefix(req->get_raw_url());
            const std::string &url = req->get_raw_url().substr(0, idx);
            auto cIt = url_connection_mapping_.find(url);
            if (cIt == url_connection_mapping_.end()) {
                resp->process_error();
                return;
            }

            if (cIt->second.empty()) {
                resp->process_error(ResponseCode::bad_gateway);
                return;
            }

            int randIdx = rand() % cIt->second.size();
            auto rIt = cIt->second.begin();
            std::advance(rIt, randIdx);
            useConn = *rIt;

            cs_connection_mapping_[conn] = useConn;
            sc_connection_mapping_[useConn] = {url, conn};
        }

        // do forwarding
        useConn->send(conn->get_input_buff(true));
        if (req->get_buff()->readable_bytes() > 0) {
            useConn->send(req->get_buff(true));
        }
    }

    void HttpProxy::put_conncetion(Connection *conn)
    {
        for (const auto &cfg : proxy_configs_) {
            if (cfg.second == conn->get_remote_address()) {
                url_connection_mapping_[cfg.first].insert(conn);
                break;
            }
        }
    }
}