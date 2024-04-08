#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iterator>

#include "buffer/pool.h"
#include "net/base/connection/connection.h"
#include "net/base/socket/inet_address.h"
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
#include "timer/timer.h"
#include "timer/timer_manager.h"

namespace net::http
{
    void RemoteConnectTask::on_timer(timer::Timer *timer)
    {
        std::cout << "connection to remote server fail ===> " << addr_.to_address_key() << '\n';
        if (resp_) {
            resp_->process_error(ResponseCode::gateway_timeout);
        }

        if (proxy_) {
            proxy_->on_connection_timeout(this);
        }
    }

    HttpProxy::HttpProxy()
    {
        server_ = nullptr;
        srand(time(nullptr));
        tasK_id_ = 0;
    }

    HttpProxy::HttpProxy(HttpServer *server) : server_(server), tasK_id_(0)
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
        auto it = sc_connection_mapping_.find(conn);
        if (it == sc_connection_mapping_.end()) {
            put_conncetion(conn);
            std::cout << "connect to server, ip: " << conn->get_remote_address().get_ip() << ", port: " << conn->get_remote_address().get_port() << " successfully\n";
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
        Buffer *buf = conn->get_input_buff(true);
        std::cout << "==============================> " << buf->readable_bytes() << '\n';
        it->second.second->write_and_flush(buf);
    }

    void HttpProxy::on_write(Connection *conn)
    {
        
    }

    void HttpProxy::on_close(Connection *conn)
    {
        std::cout << ">>>>>>>>>>>>> remote connection close: " << conn << " <<<<<<<<<<<<<\n";
        auto it = sc_connection_mapping_.find(conn);
        if (it != sc_connection_mapping_.end()) {
            if (it->second.second) {
                cs_connection_mapping_.erase(it->second.second);
            }
            sc_connection_mapping_.erase(it);
        }

        bool hasDeleted = false;
        for (auto &item : url_connection_mapping_) {
            auto cIt = item.second.find(conn);
            if (cIt != item.second.end()) {
                hasDeleted = true;
                item.second.erase(cIt);
                break;
            }
        }

        if (!hasDeleted) {
            std::cout << ">>>>>>>>>>>>> can not delete connection!!! <<<<<<<<<<<<<\n";
        }
    }

    bool HttpProxy::init_proxy_connection(const std::string &ip, short port, int taskId)
    {
        net::Socket *sock = new net::Socket(ip.c_str(), port);
        sock->set_id(taskId);
        if (!sock->valid()) {
            std::cout << "create socket fail, ip: " << ip << ", port: " << port << std::endl;
            return false;
        }

        if (!sock->connect()) {
            std::cout << " connect failed, ip: " << ip << ", port: " << port << std::endl;
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
            proxy_configs_[proxyCfg["root"]].push_back({ip, port});

            url_trie_.insert(root, true);

            url_connection_mapping_[root] = {};
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

        const std::string &key = conn->get_remote_address().to_address_key();
        auto cIt = task_client_mapping_.find(key);
        if (cIt != task_client_mapping_.end()) {
            auto taskIt = conn_tasks_.find(cIt->second);
            if (taskIt != conn_tasks_.end()) {
                taskIt->second->cancel();
                conn_tasks_.erase(taskIt);
            }
            task_client_mapping_.erase(cIt);
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
            if (!server_) {
                resp->process_error();
                return;
            }

            int idx = -url_trie_.find_prefix(req->get_raw_url());
            const std::string &url = req->get_raw_url().substr(0, idx);
            auto cIt = url_connection_mapping_.find(url);
            if (cIt == url_connection_mapping_.end()) {
                resp->process_error();
                return;
            }

            if (cIt->second.empty()) {
                auto cfgIt = proxy_configs_.find(url);
                if (cfgIt != proxy_configs_.end() && cfgIt->second.size() > 0) {
                    int taskId = gen_task_id();
                    if (taskId < 0) {
                        resp->process_error();
                        return;
                    }

                    const InetAddress &remoteAddr = cfgIt->second[rand() % cfgIt->second.size()];
                    if (!init_proxy_connection(remoteAddr.get_ip(), remoteAddr.get_port(), taskId)) {
                        resp->process_error(ResponseCode::bad_gateway);
                        return;
                    }

                    timer::TimerManager *timerManager = server_->get_timer_manager();
                    RemoteConnectTask *task = new RemoteConnectTask(taskId, req, resp, 
                        remoteAddr, this, url, conn->get_remote_address().to_address_key());
                    
                    conn_tasks_[taskId] = timerManager->timeout(5 * 1000, task);
                    task_client_mapping_[conn->get_remote_address().to_address_key()] = taskId;
                    return;
                }

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

        do_forward_packet(resp, useConn, conn->get_input_buff(true), req->get_buff(true));
    }

    void HttpProxy::put_conncetion(Connection *conn)
    {
        auto cIt = conn_tasks_.find(conn->get_scoket()->get_id());
        if (cIt != conn_tasks_.end()) {
            RemoteConnectTask *task = static_cast<RemoteConnectTask *>(cIt->second->get_task());
            cIt->second->cancel();

            Connection *clientConn = task->req_->get_context()->get_connection();
            url_connection_mapping_[task->url_].insert(conn);
            cs_connection_mapping_[clientConn] = conn;
            sc_connection_mapping_[conn] = {task->url_, clientConn};

            do_forward_packet(task->resp_, conn, clientConn->get_input_buff(true), task->req_->get_buff(true));

            conn->get_scoket()->set_id(-1);
            task_client_mapping_.erase(task->client_addr_);
            delete task;
        }
    }

    void HttpProxy::on_connection_timeout(RemoteConnectTask *task)
    {
        auto cIt = conn_tasks_.find(task->task_id_);
        if (cIt != conn_tasks_.end()) {
            RemoteConnectTask *task = static_cast<RemoteConnectTask *>(cIt->second->get_task());
            conn_tasks_.erase(cIt);
            task_client_mapping_.erase(task->client_addr_);
            delete task;
        } else {
            std::cout << "------------> internal error occured !!!\n";
        }
    }

    int HttpProxy::gen_task_id()
    {
        uint32_t id = ++tasK_id_;
        if (id >= INT32_MAX) {
            tasK_id_ = 0;
            if (conn_tasks_.count(tasK_id_)) {
                return -1;
            }
        }

        return (int)id;
    }

    void HttpProxy::do_forward_packet(HttpResponse *resp, Connection *conn, Buffer *buf1, Buffer *buf2)
    {
        if (buf1->readable_bytes() == 0) {
            std::cout << ">>>>>>>bool>>>> !!! empty data\n";
            singleton::Singleton<BufferedPool>().free(buf1);
            singleton::Singleton<BufferedPool>().free(buf2);
            resp->process_error();
        } else {
            // do forwarding
            if (buf2->readable_bytes() > 0) {
                conn->write(buf1);
                conn->write_and_flush(buf2);
            } else {
                conn->write_and_flush(buf1);
                singleton::Singleton<BufferedPool>().free(buf2);
            }
        }
    }
}