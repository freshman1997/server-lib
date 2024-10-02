#include <cassert>
#include <cstdlib>
#include <ctime>
#include <iostream>

#include "buffer/pool.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "ops/option.h"
#include "proxy.h"
#include "net/connection/tcp_connection.h"
#include "net/socket/socket.h"
#include "ops/config_manager.h"
#include "response_code.h"
#include "nlohmann/json.hpp"
#include "context.h"
#include "request.h"
#include "response.h"
#include "http_server.h"
#include "net/event/event_loop.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"

namespace net::http
{
    void RemoteConnectTask::on_timer(timer::Timer *timer)
    {
        proxy_->on_connection_timeout(this);
        resp_->process_error(ResponseCode::gateway_timeout);
    }

    bool RemoteConnectTask::need_free()
    {
        return true;
    }

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
        for (const auto &it : sc_connection_mapping_) {
            it.first->close();
            it.second->close();
        }

        cs_connection_mapping_.clear();
        sc_connection_mapping_.clear();
    }

    void HttpProxy::on_connected(Connection *conn)
    {
        auto it = sc_connection_mapping_.find(conn);
        if (it == sc_connection_mapping_.end()) {
            std::cout << "connect to server, ip: " << conn->get_remote_address().get_ip() << ", port: " << conn->get_remote_address().get_port() << " successfully\n";
            put_conncetion(conn);
        }
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
        it->second->write_and_flush(conn->get_input_buff(true));
    }

    void HttpProxy::on_write(Connection *conn)
    {
        
    }

    void HttpProxy::on_close(Connection *conn)
    {
        std::cout << ">>>>>>>>>>>>> remote connection close: " << conn << " <<<<<<<<<<<<<\n";
        auto it = sc_connection_mapping_.find(conn);
        if (it != sc_connection_mapping_.end()) {
            if (it->second) {
                cs_connection_mapping_.erase(it->second);
                it->second->close();
            }
            sc_connection_mapping_.erase(it);
        }

        auto taskIt = conn_tasks_.find(conn);
        if (taskIt != conn_tasks_.end()) {
            taskIt->second->cancel();
            RemoteConnectTask *task = dynamic_cast<RemoteConnectTask *>(taskIt->second->get_task());
            conn_tasks_.erase(taskIt);
            if (task) {
                clear_connection_pending_request(task->conn_);
                conn_tasks_.erase(task->conn_);
                task->resp_->process_error(ResponseCode::not_found);
            }
        }
    }

    Connection * HttpProxy::init_proxy_connection(const std::string &ip, short port)
    {
        net::Socket *sock = new net::Socket(ip.c_str(), port);
        if (!sock->valid()) {
            std::cout << "create socket fail, ip: " << ip << ", port: " << port << std::endl;
            return nullptr;
        }

        if (!sock->connect()) {
            std::cout << " connect failed, ip: " << ip << ", port: " << port << std::endl;
            return nullptr;
        }

        Connection *conn = new TcpConnection(sock);
        server_->get_event_loop()->on_new_connection(conn);

        conn->set_connection_handler(this);
        conn->set_event_handler(server_->get_event_loop());

        return conn;
    }

    bool HttpProxy::load_proxy_config_and_init()
    {
        auto cfgManager = HttpConfigManager::get_instance();
        if (!cfgManager->good()) {
            return false;
        }

        const auto &proxiesCfg = cfgManager->get_type_array_properties<nlohmann::json>("proxies");
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
            const std::string &host = target[0];
            short port = target[1];
            proxy_configs_[proxyCfg["root"]].push_back({host, port});

            url_trie_.insert(root, true);

            std::cout << "register http proxy, root: " << root << ", host: " << host << ", port: " << port << '\n';
        }
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
                it->second->close();
            }
            cs_connection_mapping_.erase(it);
        }
        clear_connection_pending_request(conn);
    }

    void HttpProxy::serve_proxy(HttpRequest *req, HttpResponse *resp)
    {
        Connection *conn = req->get_context()->get_connection();
        auto it = cs_connection_mapping_.find(conn);
        if (it != cs_connection_mapping_.end()) {
            do_forward_packet(resp, it->second, conn->get_input_buff(true), req->get_buff(true));
        } else {
            if (!server_) {
                resp->process_error();
                return;
            }

            auto pIt = pending_requests_.find(conn);
            if (pIt != pending_requests_.end()) {
                if (pIt->second.size() > config::proxy_max_pending) {
                    clear_connection_pending_request(conn);
                    resp->process_error();
                } else {
                    pIt->second.push_back(conn->get_input_buff(true));
                    if (req->get_body_length() > 0) {
                        pIt->second.push_back(req->get_buff(true));
                    }
                }
                return;
            }

            int idx = -url_trie_.find_prefix(req->get_raw_url(), true);
            const std::string &url = req->get_raw_url().substr(0, idx);
            auto cfgIt = proxy_configs_.find(url);
            if (cfgIt != proxy_configs_.end() && cfgIt->second.size() > 0) {
                const InetAddress &remoteAddr = cfgIt->second[rand() % cfgIt->second.size()];
                Connection *remoteConn = init_proxy_connection(remoteAddr.get_ip(), remoteAddr.get_port());
                if (!remoteConn) {
                    resp->process_error(ResponseCode::bad_gateway);
                    return;
                }

                timer::TimerManager *timerManager = server_->get_timer_manager();
                RemoteConnectTask *task = new RemoteConnectTask(conn, req, resp, 
                    remoteConn, this, url, conn->get_remote_address().to_address_key());
                
                auto timer = timerManager->timeout(config::proxy_connect_timeout, task);
                conn_tasks_[conn] = timer;
                conn_tasks_[remoteConn] = timer;

                pending_requests_[conn].push_back(conn->get_input_buff(true));
                if (req->get_body_length() > 0) {
                    pending_requests_[conn].push_back(req->get_buff(true));
                }
            } else {
                resp->process_error(ResponseCode::bad_gateway);
            }
        }
    }

    void HttpProxy::put_conncetion(Connection *conn)
    {
        auto cIt = conn_tasks_.find(conn);
        if (cIt != conn_tasks_.end()) {
            RemoteConnectTask *task = static_cast<RemoteConnectTask *>(cIt->second->get_task());
            cIt->second->cancel();

            Connection *clientConn = task->req_->get_context()->get_connection();
            cs_connection_mapping_[clientConn] = conn;
            sc_connection_mapping_[conn] = clientConn;

            auto rIt = pending_requests_.find(conn);
            if (rIt != pending_requests_.end()) {
                for (auto buf : rIt->second) {
                    conn->write(buf);
                }
                pending_requests_.erase(rIt);
            }
            delete task;
        }
    }

    void HttpProxy::on_connection_timeout(RemoteConnectTask *task)
    {
        std::cout << "connection to remote server fail ===> " << task->remote_conn_->get_remote_address().to_address_key() << '\n';
        clear_connection_pending_request(task->conn_);
        conn_tasks_.erase(task->conn_);
        task->remote_conn_->close();
    }

    void HttpProxy::do_forward_packet(HttpResponse *resp, Connection *conn, Buffer *buf1, Buffer *buf2)
    {
        if (buf1->readable_bytes() == 0) {
            std::cout << ">>>>>>>bool>>>> !!! empty data\n";
            BufferedPool::get_instance()->free(buf1);
            BufferedPool::get_instance()->free(buf2);
            resp->process_error();
        } else {
            // do forwarding
            if (buf2->readable_bytes() > 0) {
                conn->write(buf1);
                conn->write_and_flush(buf2);
            } else {
                conn->write_and_flush(buf1);
                BufferedPool::get_instance()->free(buf2);
            }
        }
    }

    void HttpProxy::clear_connection_pending_request(Connection *conn)
    {
        auto it = pending_requests_.find(conn);
        if (it != pending_requests_.end()) {
            for (auto buf : it->second) {
                BufferedPool::get_instance()->free(buf);
            }
            pending_requests_.erase(it);
        }
    }
}