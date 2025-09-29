#include "net/poller/epoll_poller.h"
#include "net/poller/poll_poller.h"
#include "net/poller/select_poller.h"
#include "net/poller/kqueue_poller.h"
#include "net/secuity/openssl.h"
#include "nlohmann/json.hpp"
#include "task/upload_file_task.h"
#include "url.h"
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <filesystem>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <winnls.h>
#include <winnt.h>
#else
#include <unistd.h>
#endif

#include "timer/wheel_timer_manager.h"
#include "net/acceptor/tcp_acceptor.h"
#include "event/event_loop.h"
#include "http_server.h"
#include "net/socket/socket.h"
#include "request.h"
#include "session.h"
#include "response_code.h"
#include "ops/config_manager.h"
#include "ops/option.h"
#include "response.h"
#include "content_type.h"
#include "header_key.h"
#include "header_util.h"
#include "proxy.h"

namespace yuan::net::http
{
    HttpServer::HttpServer() : quit_(false), state_(State::invalid), poller_(nullptr), acceptor_(nullptr), event_loop_(nullptr), timer_manager_(nullptr), ssl_module_(nullptr), proxy_(nullptr)
    {
        
    }

    HttpServer::~HttpServer()
    {
        delete poller_;
        delete acceptor_;
        delete proxy_;
        for (const auto &val : sessions_ | std::views::values) {
            delete val;
        }

        sessions_.clear();
    }

    void HttpServer::on_connected(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        if (sessions_.count(sessionId)) {
            std::cerr << "session already exists!!!\n";
            return;
        }

        sessions_[sessionId] = new HttpSession(sessionId, new HttpSessionContext(conn), timer_manager_);
    }

    void HttpServer::on_error(Connection *conn)
    {
        
    }

    void HttpServer::on_read(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            std::cout << "----------------> error\n";
            return;
        }

        auto session = it->second;
        auto context = session->get_context();
        if (!context->parse()) {
            if (context->has_error()) {
                context->process_error(context->get_error_code());
            }
            return;
        }

        if (context->has_error()) {
            context->process_error(context->get_error_code());
            return;
        }

        if (context->is_completed()) {
            if (proxy_ && proxy_->is_proxy(context->get_request()->get_raw_url())) {
                proxy_->serve_proxy(context->get_request(), context->get_response());
            } else {
                if (!context->try_parse_request_content()) {
                    context->process_error(ResponseCode::bad_request);
                    return;
                }

                if (!context->is_completed()) {
                    return;
                }

                auto handler = dispatcher_.get_handler(context->get_request()->get_raw_url());
                if (handler) {
                    handler(context->get_request(), context->get_response());
                } else {
                    // 404
                    context->process_error(ResponseCode::not_found);
                }
            }

            if (config::close_idle_connection && sessions_.count(sessionId)) {
                session->reset_timer();
            }
        }
    }

    void HttpServer::on_write(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            std::cout << "----------------> error\n";
            return;
        }

        auto context = it->second->get_context();
        context->write();
    }

    void HttpServer::on_close(Connection *conn)
    {
        free_session(conn);
    }

    bool HttpServer::init(int port)
    {
        if (HttpConfigManager::get_instance()->good()) {
            std::cout << HttpConfigManager::get_instance()->get_string_property("server_name") << " starting...\n";
        }

        net::Socket *sock = new net::Socket("", port);
        if (!sock->valid()) {
            std::cerr << "create socket fail!!! " << errno << "\n";
            delete sock;
            sock = nullptr;
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);
        if (!sock->bind()) {
            std::cerr << " bind port " << port << " failed!!! " << std::endl;
            delete sock;
            sock = nullptr;
            return false;
        }

    #ifdef __unix__
        poller_ = new net::EpollPoller;
    #elif defined _WIN32
        poller_ = new net::SelectPoller;
    #elif defined __APPLE__
        poller_ = new net::KQueuePoller;
    #endif

        if (!poller_->init()) {
            std::cerr << " poller init failed!!! " << std::endl;
            delete sock;
            sock = nullptr;
            return false;
        }

        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cerr << " listen failed!!! " << std::endl;
            return false;
        }

    #ifdef HTTP_USE_SSL
        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init("./ssl/ca.crt", "./ssl/ca.key", SSLHandler::SSLMode::acceptor_)) {
            if (auto msg = ssl_module_->get_error_message()) {
                std::cerr << msg->c_str() << '\n';
            }
            return false;
        }

        acceptor_->set_ssl_module(ssl_module_);
    #endif
        config::load_config();
        load_static_paths();

        // 注册 reload_config 事件
        on("/reload_config", std::bind(&HttpServer::reload_config, this, std::placeholders::_1, std::placeholders::_2));

        const auto &proxiesCfg = HttpConfigManager::get_instance()->get_type_array_properties<nlohmann::json>("proxies");
        if (!proxiesCfg.empty()) {
            proxy_ = new HttpProxy(this);
            if (!proxy_->load_proxy_config_and_init()) {
                std::cerr << " load proxies config failed!!! " << std::endl;
                return false;
            }
        }

        return true;
    }

    void HttpServer::serve()
    {
        timer::WheelTimerManager timerManager;
        timer_manager_ = &timerManager;

        net::EventLoop loop(poller_, &timerManager);
        acceptor_->set_event_handler(&loop);
        TcpAcceptor *acceptor = static_cast<TcpAcceptor *>(acceptor_);
        acceptor->set_connection_handler(this);

        loop.update_channel(acceptor_->get_channel());
        this->event_loop_ = &loop;

        std::cout << HttpConfigManager::get_instance()->get_string_property("server_name", "web server") << " started\n";

        loop.loop();
    }

    void HttpServer::stop()
    {
        event_loop_->quit();
    }

    void HttpServer::on(const std::string &url, request_function func, bool is_prefix)
    {
        if (url.empty() || !func) {
            return;
        }

        dispatcher_.register_handler(url, func, is_prefix);
    }

    void HttpServer::free_session(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            if (proxy_) {
                proxy_->on_client_close(conn);
            }

            delete it->second;
            sessions_.erase(it);
        } else {
            std::cerr << "internal error found!!!\n";
        }
    }

    void HttpServer::load_static_paths()
    {
        // 必须要支持 /static/* 这种形式
        auto cfgManager = HttpConfigManager::get_instance();
        const std::vector<nlohmann::json> &paths = cfgManager->get_type_array_properties<nlohmann::json>(config::static_file_paths);
        for (const auto &path : paths) {
            const std::string &root = path[config::static_file_paths_root];
            const std::string &rootPath = path[config::static_file_paths_path];
            if (root.empty() || rootPath.empty()) {
                continue;
            }

            static_paths_[root] = rootPath;
            on(root, std::bind(&HttpServer::serve_static, this, std::placeholders::_1, std::placeholders::_2), true);
        }

        if (paths.empty()) {
            static_paths_["/static"] = std::filesystem::current_path().string();
            on("/static", std::bind(&HttpServer::serve_static, this, std::placeholders::_1, std::placeholders::_2), true);
        }

        const std::vector<std::string> &types = cfgManager->get_type_array_properties<std::string>(config::playable_types);
        for (const auto &type : types) {
            play_types_.insert(type);
        }

        if (play_types_.empty()) {
            play_types_.insert(".mp4");
            play_types_.insert(".mp3");
            play_types_.insert(".mov");
            play_types_.insert(".flac");
            play_types_.insert(".wav");
            play_types_.insert(".avi");
            play_types_.insert(".ogg");
        }

    }

    void HttpServer::serve_static(HttpRequest *req, HttpResponse *resp)
    {
        const std::string &url = req->get_raw_url();
        const int prefixIdx = -dispatcher_.get_compress_trie().find_prefix(url, true);
        const std::string prefix = url.substr(0, prefixIdx);
        const std::string &pathPrefix = static_paths_[prefix];
        if (pathPrefix.empty()) {
            resp->process_error();
            return;
        }

        if (prefixIdx + 1 > url.size()) {
            resp->process_error(ResponseCode::not_found);
            return;
        }

        if (url.size() == prefixIdx + 1) {
            // 访问的是根目录
            serve_list_files("/static/", pathPrefix, resp);
            return;
        }

        const std::string &fileRelativePath = url.substr(prefixIdx + 1);
        if (fileRelativePath == "filelist.html") {
            resp->get_buff()->write_string(config::file_list_html_text.data(), config::file_list_html_text.size());
            resp->add_header("Content-Type", "text/html; charset=utf-8");
            resp->set_response_code(ResponseCode::ok_);
            resp->add_header("Connection", "close");
            resp->add_header("Content-Length", std::to_string(resp->get_buff()->readable_bytes()));
            resp->send();
            resp->get_context()->get_connection()->close();
            return;
        }

        std::string path;
        if (pathPrefix[pathPrefix.size() - 1] == '/') {
            path = pathPrefix + fileRelativePath;
        } else {
            path = pathPrefix + "/" + fileRelativePath;
        }

        if (std::filesystem::is_directory(std::u8string(path.begin(), path.end()))) {
            serve_list_files(url, path, resp);
            return;
        }

        const std::size_t pos = fileRelativePath.find_last_of('.');
        if (pos == std::string::npos) {
            serve_download(path, "", resp);
            return;
        }

        const std::string &ext = fileRelativePath.substr(pos);
        auto refererKey = req->get_header("referer");
        if (!refererKey || !play_types_.count(ext)) {
            serve_download(path, ext, resp);
            return;
        }

        auto *session = req->get_context()->get_session();
        const SessionItem *data = session->get_session_value(path);
        std::ifstream *stream;
        if (!data) {
#ifdef _WIN32
            auto *file_ = new std::ifstream(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::in | std::ios::binary);
#else
            auto *file_ = new std::ifstream(path.c_str(), std::ios::in | std::ios::binary);
#endif
            if (!file_->good()) {
                std::cout << "open file fail! " << errno << '\n';
                resp->process_error(ResponseCode::not_found);
                file_->close();
                delete file_;
                return;
            }

            session->add_session_value(path, file_);
            stream = file_;
            session->set_close_call_back([fileRelativePath](HttpSession *httpSession) {
                const auto &items = httpSession->get_session_values();
                for (const auto &item : items) {
                    if (item.second.type == SessionItemType::pval && item.second.number.pval) {
                        auto *stream = static_cast<std::ifstream *>(item.second.number.pval);
                        stream->close();
                        delete stream;
                    }
                }
            });
        } else {
            stream = static_cast<std::ifstream *>(data->number.pval);
        }

        const std::size_t contentSize = config::client_max_content_length;
        std::size_t length;
        const std::string lenKey = path + "_length";
        if (const SessionItem *lenItem = session->get_session_value(lenKey); !lenItem) {
            stream->seekg(0, std::ios_base::end);
            length = stream->tellg();
            session->add_session_value(lenKey, length);
        } else {
            length = lenItem->number.sz_val;
        }

        const std::string &contentType = get_content_type(ext);
        if (length == 0 || contentType.empty()) {
            resp->process_error(ResponseCode::bad_request);
            return;
        }

        std::size_t offset = 0;

        if (const std::string *range = req->get_header(net::http::http_header_key::range)) {
            const auto &ranges = helper::parse_range(*range);
            if (ranges.empty()) {
                resp->process_error(ResponseCode::bad_request);
                return;
            }
            offset = ranges[0].first;
        }

        std::string bytes = "bytes ";
        bytes.append(std::to_string(offset))
            .append("-")
            .append(std::to_string(length - 1))
            .append("/")
            .append(std::to_string(length));
        
        resp->add_header("Content-Type", contentType);
        resp->add_header("Content-Range", bytes);
        resp->add_header("Content-Dispostion", "inline; filename=\"" + url::url_encode(fileRelativePath) + "\"");
        std::size_t sz = contentSize + offset > length ? length - offset : contentSize;
        resp->add_header("Content-length", std::to_string(sz));

        stream->seekg(offset, std::ios::beg);
        if (resp->get_buff()->writable_size() < sz) {
            resp->get_buff()->resize(sz);
        }

        stream->read(resp->get_buff()->buffer_begin(), sz);
        resp->get_buff()->fill(sz);

        if (stream->eof()) {
            stream->clear();
        }

        resp->add_header("Accept-Ranges", "bytes");
        resp->add_header("X-Content-Type-Options", "nosniff");
        resp->add_header("Cache-Control", "no-cache");
        resp->set_response_code(net::http::ResponseCode::partial_content);
        resp->send();
    }

    void HttpServer::serve_download(const std::string &filePath, const std::string &ext, HttpResponse *resp)
    {
        std::fstream file;
        file.open(std::filesystem::path(std::u8string(filePath.begin(), filePath.end())), std::ios::in | std::ios::binary);
        if (!file.good()) {
            resp->get_context()->process_error(net::http::ResponseCode::not_found);
            return;
        }

        file.seekg(0, std::ios_base::end);
        std::size_t sz = file.tellg();
        file.close();

        net::http::HttpUploadFileTask *task = new net::http::HttpUploadFileTask([resp, filePath]() {
            std::cout << "Upload file task completed, file path: " << filePath << '\n';
            resp->set_upload_file(false);
        });

        auto attachment_info = std::make_shared<net::http::AttachmentInfo>();
        attachment_info->origin_file_name_ = filePath;
        attachment_info->length_ = sz;
        task->set_attachment_info(attachment_info);

        if (!task->init()) {
            resp->get_context()->process_error(net::http::ResponseCode::internal_server_error);
            delete task;
            return;
        }

        resp->add_header("Content-Type", get_content_type(ext));
        resp->add_header("Connection", "close");
        resp->set_response_code(net::http::ResponseCode::ok_);
        resp->add_header("Content-Length", std::to_string(sz));

        resp->set_task(task);
        resp->set_upload_file(true);
        resp->send();
    }

    void HttpServer::serve_list_files(const std::string &relPath, const std::string &filePath, HttpResponse *resp)
    {
        nlohmann::json jsonResponse;
        std::vector<std::string> files;
        try {
            for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(std::u8string(filePath.begin(), filePath.end())))) {
                if (entry.is_regular_file())
                {
                    nlohmann::json item;
                    item["type"] = 1;
                    item["name"] = entry.path().filename().string();
                    item["size"] = std::filesystem::file_size(entry);
                    auto sys_time = std::chrono::file_clock::to_sys(entry.last_write_time());
                    item["modified"] = std::chrono::system_clock::to_time_t(sys_time); // 转换为时间戳
                    item["url"] = relPath + entry.path().filename().string();
                    jsonResponse.push_back(item);
                }
                else if (entry.is_directory())
                {
                    nlohmann::json item;
                    item["name"] = entry.path().filename().string();
                    item["type"] = 2;
                    auto sys_time =  std::chrono::file_clock::to_sys(entry.last_write_time());
                    item["modified"] = std::chrono::system_clock::to_time_t(sys_time); // 转换为时间戳
                    item["url"] = relPath + entry.path().filename().string() + "/";
                    jsonResponse.push_back(item);
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Error reading directory: " << e.what() << '\n';
            resp->process_error();
            return;
        }

        resp->add_header("Content-Type", "application/json");
        resp->add_header("Connection", "close");
        resp->append_body(jsonResponse.dump());
        resp->add_header("Content-Length", std::to_string(resp->get_buff()->readable_bytes()));
        resp->set_response_code(net::http::ResponseCode::ok_);
        resp->send();
    }

    void HttpServer::reload_config(HttpRequest *req, HttpResponse *resp)
    {
        if (HttpConfigManager::get_instance()->reload_config()) {
            load_static_paths();
            resp->get_buff()->write_string("Configuration reloaded successfully.");
        } else {
            resp->get_buff()->write_string("Failed to reload configuration.");
        }

        resp->add_header("Content-Type", "text/plain; charset=utf-8");
        resp->add_header("Connection", "close");
        resp->set_response_code(net::http::ResponseCode::ok_);
        resp->add_header("Content-Length", std::to_string(resp->get_buff()->readable_bytes()));
        resp->send();
    }
}