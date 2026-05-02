#include "http_service.h"
#include "request.h"
#include "response.h"
#include "app_events.h"
#include "service_registry.h"
#include "bit_torrent_service.h"
#include "proxy.h"
#include "reverse_proxy.h"
#include "server_service_custom_events.h"
#include "content/types.h"
#include "peer_wire/peer_connection.h"
#include "utils.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#if __has_include("proxy/websocket_proxy.h")
#include "proxy/websocket_proxy.h"
#define YUAN_HAS_WEBSOCKET_PROXY 1
#endif

namespace yuan::server
{
    namespace
    {
        constexpr size_t kDashboardRecentEventLimit = 120;
        constexpr size_t kBtTaskHistoryLimit = 50;

        struct BtTaskHistoryItem
        {
            uint64_t ts = 0;
            std::string action;
            std::string info_hash;
            std::string name;
            std::string save_path;
            bool running = false;
        };

        std::mutex g_bt_history_mutex;
        std::deque<BtTaskHistoryItem> g_bt_task_history;

        std::filesystem::path bt_history_file_path()
        {
            return std::filesystem::path("bt_downloader_task_history.json");
        }

        void persist_bt_task_history_locked()
        {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto &item : g_bt_task_history) {
                nlohmann::json j;
                j["ts"] = item.ts;
                j["action"] = item.action;
                j["info_hash"] = item.info_hash;
                j["name"] = item.name;
                j["save_path"] = item.save_path;
                j["running"] = item.running;
                arr.push_back(std::move(j));
            }

            std::ofstream out(bt_history_file_path(), std::ios::binary | std::ios::trunc);
            if (!out.good()) {
                return;
            }
            out << arr.dump(2);
        }

        void load_bt_task_history_from_file()
        {
            std::ifstream in(bt_history_file_path(), std::ios::binary);
            if (!in.good()) {
                return;
            }

            nlohmann::json arr;
            try {
                in >> arr;
            } catch (...) {
                return;
            }

            if (!arr.is_array()) {
                return;
            }

            std::lock_guard<std::mutex> lock(g_bt_history_mutex);
            g_bt_task_history.clear();
            for (const auto &j : arr) {
                BtTaskHistoryItem item;
                item.ts = j.value("ts", static_cast<uint64_t>(0));
                item.action = j.value("action", std::string());
                item.info_hash = j.value("info_hash", std::string());
                item.name = j.value("name", std::string());
                item.save_path = j.value("save_path", std::string());
                item.running = j.value("running", false);
                g_bt_task_history.push_back(std::move(item));
            }

            while (g_bt_task_history.size() > kBtTaskHistoryLimit) {
                g_bt_task_history.pop_front();
            }
        }

        void append_bt_task_history(BtTaskHistoryItem item)
        {
            std::lock_guard<std::mutex> lock(g_bt_history_mutex);
            g_bt_task_history.push_back(std::move(item));
            while (g_bt_task_history.size() > kBtTaskHistoryLimit) {
                g_bt_task_history.pop_front();
            }
            persist_bt_task_history_locked();
        }

        nlohmann::json build_bt_task_history_json()
        {
            nlohmann::json arr = nlohmann::json::array();
            std::lock_guard<std::mutex> lock(g_bt_history_mutex);
            for (const auto &item : g_bt_task_history) {
                nlohmann::json j;
                j["ts"] = item.ts;
                j["action"] = item.action;
                j["info_hash"] = item.info_hash;
                j["name"] = item.name;
                j["save_path"] = item.save_path;
                j["running"] = item.running;
                arr.push_back(std::move(j));
            }
            return arr;
        }

        std::string request_header(yuan::net::http::HttpRequest *req, const char *name)
        {
            if (!req) {
                return {};
            }
            const auto *value = req->get_header(name);
            return value ? *value : std::string();
        }

        std::string request_body(yuan::net::http::HttpRequest *req)
        {
            if (!req) {
                return {};
            }
            if (req->body_buffer_size() > 0) {
                return req->body_buffer_text();
            }
            const char *begin = req->body_begin();
            const char *end = req->body_end();
            if (!begin || !end || end < begin) {
                return {};
            }
            return std::string(begin, end);
        }

        uint64_t now_ms()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        bool read_text_file(const std::filesystem::path &path, std::string &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.good()) {
                return false;
            }
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !out.empty();
        }

        bool load_admin_asset(std::string_view file_name, std::string &out)
        {
            const std::vector<std::filesystem::path> candidates = {
                std::filesystem::path("web/admin") / std::string(file_name),
                std::filesystem::path("../web/admin") / std::string(file_name),
                std::filesystem::path("../../web/admin") / std::string(file_name),
                std::filesystem::path("server/bt_downloader/web/admin") / std::string(file_name),
                std::filesystem::path("../server/bt_downloader/web/admin") / std::string(file_name),
                std::filesystem::path("../../server/bt_downloader/web/admin") / std::string(file_name),
                std::filesystem::path("../../../server/bt_downloader/web/admin") / std::string(file_name),
            };

            for (const auto &path : candidates) {
                if (read_text_file(path, out)) {
                    return true;
                }
            }
            return false;
        }

        void reply_asset(yuan::net::http::HttpResponse *resp,
                         const std::string &content,
                         const char *content_type)
        {
            resp->set_response_code(yuan::net::http::ResponseCode::ok_);
            resp->add_header("Content-Type", content_type);
            resp->append_body(content);
            resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
            resp->send();
        }

        nlohmann::json build_task_stats_json(
            const yuan::server::BitTorrentService::ManagedTask &task,
            const yuan::net::bit_torrent::BitTorrentClient *client)
        {
            nlohmann::json item;
            item["id"] = task.id;
            item["torrent_path"] = task.torrent_path;
            item["magnet_uri"] = task.magnet_uri;
            item["save_path"] = task.save_path;
            item["info_hash"] = task.info_hash;
            item["name"] = task.name;
            item["total_bytes"] = task.total_size;
            item["status"] = task.status;
            item["last_error"] = task.last_error;
            item["updated_at_ms"] = task.updated_at_ms;
            item["active"] = (client != nullptr);
            item["running"] = (client != nullptr && client->is_running());
            item["has_torrent_data"] = task.has_torrent_data;
            if (client && client->has_loaded_torrent()) {
                const auto &stats = client->get_stats();
                item["progress"] = stats.progress_;
                item["peer_count"] = client->get_peer_count();
                item["download_speed"] = stats.download_speed_;
                item["downloaded_bytes"] = stats.downloaded_bytes_;
                item["uploaded_bytes"] = stats.uploaded_bytes_;
                item["pieces_downloaded"] = stats.pieces_downloaded_;
                item["pieces_total"] = stats.total_pieces_;
            } else {
                item["progress"] = 0.0f;
                item["peer_count"] = 0;
                item["download_speed"] = 0.0f;
                item["downloaded_bytes"] = 0;
                item["uploaded_bytes"] = 0;
                item["pieces_downloaded"] = 0;
                item["pieces_total"] = 0;
            }
            return item;
        }
    }


    HttpService::HttpService(int port, yuan::net::http::HttpServerConfig config)
        : port_(port), config_(std::move(config)), server_(std::make_unique<yuan::net::http::HttpServer>(config_)), host_({ "http", "http", port })
    {
        server_->set_proxy_factory(yuan::net::http::create_http_proxy_handler);
    }

    HttpService::~HttpService()
    {
        stop();
    }

    bool HttpService::init()
    {
        if (!server_) {
            return false;
        }

        bool ok = false;
        if (shared_runtime_) {
            ok = server_->init(port_, *shared_runtime_);
        } else {
            ok = server_->init(port_);
        }

        if (ok) {
#ifdef YUAN_HAS_WEBSOCKET_PROXY
            auto *proxy = server_->get_proxy();
            if (proxy) {
                yuan::net::websocket::WebSocketProxy::install(*server_, *proxy);
            }
#endif
            load_bt_task_history_from_file();
            install_admin_dashboard_routes();
            subscribe_dashboard_events();
        }

        return ok;
    }

    void HttpService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        runtime_context_ = context;
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void HttpService::start()
    {
        host_.start([this]() { server_->serve(); });
    }

    void HttpService::stop()
    {
        unsubscribe_dashboard_events();
        host_.stop([this]() { server_->stop(); });
    }

    yuan::net::http::HttpServer &HttpService::server()
    {
        return *server_;
    }

    const yuan::net::http::HttpServer &HttpService::server() const
    {
        return *server_;
    }

    void HttpService::install_admin_dashboard_routes()
    {
        if (!server_) {
            return;
        }

        server_->on("/admin", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            std::string html;
            if (!load_admin_asset("dashboard.html", html)) {
                resp->json("{\"error\":\"admin_dashboard_asset_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }
            (void)this;
            reply_asset(resp, html, "text/html; charset=utf-8");
        });

        server_->on("/admin/assets/dashboard.css", [](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            std::string css;
            if (!load_admin_asset("dashboard.css", css)) {
                resp->json("{\"error\":\"admin_dashboard_css_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            reply_asset(resp, css, "text/css; charset=utf-8");
        });

        server_->on("/admin/assets/dashboard.js", [](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            std::string js;
            if (!load_admin_asset("dashboard.js", js)) {
                resp->json("{\"error\":\"admin_dashboard_js_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            reply_asset(resp, js, "application/javascript; charset=utf-8");
        });

        server_->on("/admin/api/overview", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            nlohmann::json out;
            out["app_name"] = runtime_context_.app_name;
            out["worker_index"] = runtime_context_.worker_index;
            out["service_count"] = runtime_context_.service_registry ? runtime_context_.service_registry->list_services().size() : 0;

            {
                std::lock_guard<std::mutex> lock(dashboard_mutex_);
                out["last_event_ms"] = dashboard_snapshot_.last_event_ms;
                out["event_counters"] = event_counters_;
                out["service_states"] = service_states_;

                nlohmann::json recent = nlohmann::json::array();
                for (const auto &evt : recent_events_) {
                    nlohmann::json item;
                    item["ts"] = evt.timestamp_ms;
                    item["name"] = evt.name;
                    item["detail"] = evt.detail;
                    recent.push_back(std::move(item));
                }
                out["recent_events"] = std::move(recent);
            }

            nlohmann::json bt;
            bt["running"] = false;
            bt["complete"] = false;
            bt["peer_count"] = 0;
            bt["downloaded_bytes"] = 0;
            bt["uploaded_bytes"] = 0;
            bt["pieces_downloaded"] = 0;
            bt["pieces_total"] = 0;
            bt["progress"] = 0.0;

            if (runtime_context_.service_registry) {
                auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
                if (!bt_service) {
                    bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
                }
                if (bt_service) {
                    const auto &clients = bt_service->active_clients();

                    int32_t total_peers = 0;
                    int64_t total_downloaded = 0;
                    int64_t total_uploaded = 0;
                    double total_speed = 0.0;
                    bool any_running = false;
                    bool any_complete = false;
                    double sum_progress = 0.0;
                    int32_t running_count = 0;
                    int32_t total_pieces_downloaded = 0;
                    int32_t total_pieces_total = 0;

                    for (const auto &pair : clients) {
                        auto &client = pair.second;
                        if (!client) continue;
                        const auto stats = client->get_stats();
                        total_peers += client->get_peer_count();
                        total_downloaded += stats.downloaded_bytes_;
                        total_uploaded += stats.uploaded_bytes_;
                        total_speed += stats.download_speed_;
                        total_pieces_downloaded += stats.pieces_downloaded_;
                        total_pieces_total += stats.total_pieces_;
                        if (client->is_running()) any_running = true;
                        if (client->is_complete()) any_complete = true;
                        sum_progress += stats.progress_;
                        ++running_count;

                        {
                            std::lock_guard<std::mutex> lock(dashboard_mutex_);
                            ++dashboard_snapshot_.bt_speed_samples;
                            dashboard_snapshot_.bt_speed_download_sum += stats.download_speed_;
                        }
                    }

                    bt["running"] = any_running;
                    bt["complete"] = any_complete;
                    bt["peer_count"] = total_peers;
                    bt["downloaded_bytes"] = total_downloaded;
                    bt["uploaded_bytes"] = total_uploaded;
                    bt["download_speed"] = total_speed;
                    bt["pieces_downloaded"] = total_pieces_downloaded;
                    bt["pieces_total"] = total_pieces_total;
                    bt["progress"] = running_count > 0 ? (sum_progress / running_count) : 0.0;
                    bt["active_count"] = running_count;
                    bt["max_concurrent"] = bt_service->max_concurrent_downloads();

                    if (!clients.empty()) {
                        auto &first_client = clients.begin()->second;
                        bt["max_peers"] = first_client->get_max_peers();
                        bt["listen_port"] = first_client->get_listen_port();
                        bt["listen_port_end"] = bt_service->default_listen_port_end();
                        bt["download_limit_kbps"] = first_client->get_download_limit_kbps();
                        bt["upload_limit_kbps"] = first_client->get_upload_limit_kbps();
                        bt["download_limit_active"] = first_client->get_download_limit_kbps() > 0;
                        bt["upload_limit_active"] = first_client->get_upload_limit_kbps() > 0;
                        const auto &nat_cfg = first_client->get_nat_config();
                        bt["enable_dht"] = nat_cfg.enable_dht;
                        bt["enable_pex"] = nat_cfg.enable_pex;
                        bt["enable_upnp"] = nat_cfg.enable_upnp;
                        bt["metadata_mode"] = first_client->is_metadata_mode();
                    } else {
                        bt["max_peers"] = bt_service->default_max_peers();
                        bt["listen_port"] = bt_service->default_listen_port();
                        bt["listen_port_end"] = bt_service->default_listen_port_end();
                        bt["download_limit_kbps"] = bt_service->default_download_limit_kbps();
                        bt["upload_limit_kbps"] = bt_service->default_upload_limit_kbps();
                        bt["download_limit_active"] = bt_service->default_download_limit_kbps() > 0;
                        bt["upload_limit_active"] = bt_service->default_upload_limit_kbps() > 0;
                        const auto &nat_cfg = bt_service->default_nat_config();
                        bt["enable_dht"] = nat_cfg.enable_dht;
                        bt["enable_pex"] = nat_cfg.enable_pex;
                        bt["enable_upnp"] = nat_cfg.enable_upnp;
                        bt["metadata_mode"] = false;
                    }

                    nlohmann::json tasks = nlohmann::json::array();
                    for (const auto &task : bt_service->list_tasks()) {
                        auto client = bt_service->get_client_by_task_id(task.id);
                        tasks.push_back(build_task_stats_json(task, client.get()));
                    }
                    bt["tasks"] = std::move(tasks);
                }
            }

            {
                std::lock_guard<std::mutex> lock(dashboard_mutex_);
                bt["peer_connected_total"] = dashboard_snapshot_.bt_peer_connected_total;
                bt["piece_completed_total"] = dashboard_snapshot_.bt_piece_completed_total;
                bt["piece_completed_bytes"] = dashboard_snapshot_.bt_piece_completed_bytes;
                bt["torrent_completed_total"] = dashboard_snapshot_.bt_torrent_completed_total;
                bt["last_info_hash"] = dashboard_snapshot_.bt_last_info_hash;
                bt["last_torrent_name"] = dashboard_snapshot_.bt_last_torrent_name;

                double avg_download = 0.0;
                double avg_upload = 0.0;
                if (dashboard_snapshot_.bt_speed_samples > 0) {
                    avg_download = dashboard_snapshot_.bt_speed_download_sum / static_cast<double>(dashboard_snapshot_.bt_speed_samples);
                }
                bt["avg_download_speed"] = avg_download;
                bt["avg_upload_speed"] = avg_upload;
            }

            out["bt"] = std::move(bt);
            out["bt_task_history"] = build_bt_task_history_json();
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/settings", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            if (input.contains("max_concurrent_downloads") && input["max_concurrent_downloads"].is_number_integer()) {
                int32_t old_max = bt_service->max_concurrent_downloads();
                bt_service->set_max_concurrent_downloads(input["max_concurrent_downloads"].get<int>());
                if (input["max_concurrent_downloads"].get<int>() > old_max) {
                    bt_service->try_start_queued_tasks();
                }
            }

            if (input.contains("max_peers") && input["max_peers"].is_number_integer()) {
                bt_service->set_default_max_peers(input["max_peers"].get<int>());
            }
            if (input.contains("listen_port") && input["listen_port"].is_number_integer()) {
                bt_service->set_default_listen_port(input["listen_port"].get<int>());
            }
            if (input.contains("listen_port_end") && input["listen_port_end"].is_number_integer()) {
                bt_service->set_default_listen_port_end(input["listen_port_end"].get<int>());
            }
            if (input.contains("download_limit_kbps") && input["download_limit_kbps"].is_number_integer()) {
                bt_service->set_default_download_limit_kbps(input["download_limit_kbps"].get<int>());
            }
            if (input.contains("upload_limit_kbps") && input["upload_limit_kbps"].is_number_integer()) {
                bt_service->set_default_upload_limit_kbps(input["upload_limit_kbps"].get<int>());
            }

            bool nat_changed = false;
            yuan::net::bit_torrent::NatConfig nat_cfg;
            bool has_nat_cfg = false;

            auto clients = bt_service->active_clients();
            for (auto &pair : clients) {
                auto client = pair.second;
                if (!client) continue;

                if (input.contains("max_peers") && input["max_peers"].is_number_integer()) {
                    client->set_max_peers(input["max_peers"].get<int>());
                }
                if (input.contains("download_limit_kbps") && input["download_limit_kbps"].is_number_integer()) {
                    client->set_download_limit_kbps(input["download_limit_kbps"].get<int>());
                }
                if (input.contains("upload_limit_kbps") && input["upload_limit_kbps"].is_number_integer()) {
                    client->set_upload_limit_kbps(input["upload_limit_kbps"].get<int>());
                }

                if (!has_nat_cfg) {
                    nat_cfg = client->get_nat_config();
                    has_nat_cfg = true;
                }
            }

            if (!has_nat_cfg) {
                nat_cfg = yuan::net::bit_torrent::NatConfig();
                has_nat_cfg = true;
            }

            if (input.contains("enable_dht") && input["enable_dht"].is_boolean()) {
                nat_cfg.enable_dht = input["enable_dht"].get<bool>();
                nat_changed = true;
            }
            if (input.contains("enable_pex") && input["enable_pex"].is_boolean()) {
                nat_cfg.enable_pex = input["enable_pex"].get<bool>();
                nat_changed = true;
            }
            if (input.contains("enable_upnp") && input["enable_upnp"].is_boolean()) {
                nat_cfg.enable_upnp = input["enable_upnp"].get<bool>();
                nat_cfg.enable_nat_pmp = input["enable_upnp"].get<bool>();
                nat_changed = true;
            }

            if (nat_changed) {
                for (auto &pair : clients) {
                    if (pair.second) {
                        pair.second->set_nat_config(nat_cfg);
                    }
                }
                bt_service->set_default_nat_config(nat_cfg);
            }

            nlohmann::json out;
            out["ok"] = true;
            out["message"] = "BitTorrent settings updated";
            out["max_concurrent_downloads"] = bt_service->max_concurrent_downloads();
            out["listen_port_end"] = bt_service->default_listen_port_end();
            if (!clients.empty()) {
                auto &first = clients.begin()->second;
                out["applied_max_peers"] = input.value("max_peers", 0);
                out["applied_listen_port"] = input.value("listen_port", 0);
                out["applied_download_limit_kbps"] = first->get_download_limit_kbps();
                out["applied_upload_limit_kbps"] = first->get_upload_limit_kbps();
                out["download_limit_active"] = first->get_download_limit_kbps() > 0;
                out["upload_limit_active"] = first->get_upload_limit_kbps() > 0;
            }
            if (nat_changed && has_nat_cfg) {
                out["applied_enable_dht"] = nat_cfg.enable_dht;
                out["applied_enable_pex"] = nat_cfg.enable_pex;
                out["applied_enable_upnp"] = nat_cfg.enable_upnp;
                if (bt_service->active_task_count() > 0) {
                    out["nat_restart_required"] = true;
                    out["hint"] = "NAT/DHT/PEX/UPnP changes take effect after restarting the active task";
                }
            }
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/history", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            int page = req->get_param_int("page", 1);
            int page_size = req->get_param_int("page_size", 20);
            if (page < 1) {
                page = 1;
            }
            if (page_size < 1) {
                page_size = 1;
            }
            if (page_size > 100) {
                page_size = 100;
            }

            nlohmann::json all = build_bt_task_history_json();
            const int total = static_cast<int>(all.size());
            const int start = (page - 1) * page_size;

            nlohmann::json items = nlohmann::json::array();
            if (start < total) {
                const int end = std::min(total, start + page_size);
                for (int i = start; i < end; ++i) {
                    items.push_back(all[i]);
                }
            }

            nlohmann::json out;
            out["page"] = page;
            out["page_size"] = page_size;
            out["total"] = total;
            out["items"] = std::move(items);
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/task", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::delete_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            if (bt_service->active_task_count() == 0) {
                resp->json("{\"error\":\"no_active_task\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto &clients = bt_service->active_clients();
            if (clients.empty()) {
                resp->json("{\"error\":\"no_active_task\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            int64_t first_task_id = clients.begin()->first;
            auto client = bt_service->get_client_by_task_id(first_task_id);
            std::string info_hash, name, save_path;
            if (client) {
                info_hash = client->get_meta().info_hash_hex_;
                name = client->get_meta().info.name_;
                save_path = client->get_save_path();
            }

            bt_service->remove_task(first_task_id);

            append_bt_task_history(BtTaskHistoryItem{
                now_ms(),
                "remove",
                info_hash,
                name,
                save_path,
                false,
            });

            {
                std::lock_guard<std::mutex> lock(dashboard_mutex_);
                dashboard_snapshot_.bt_last_info_hash.clear();
                dashboard_snapshot_.bt_last_torrent_name.clear();
            }

            nlohmann::json out;
            out["ok"] = true;
            out["message"] = "BitTorrent task stopped";
            out["task_id"] = first_task_id;
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/control", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            const std::string body = request_body(req);

            nlohmann::json input;
            try {
                input = body.empty() ? nlohmann::json::object() : nlohmann::json::parse(body);
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto action = input.value("action", "");
            if (action != "start" && action != "stop") {
                resp->json("{\"error\":\"unsupported_action\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json out;
            if (action == "start") {
                int64_t start_id = 0;
                for (const auto &t : bt_service->list_tasks()) {
                    if (t.status == "stopped" || t.status == "queued" || t.status == "error") {
                        start_id = t.id;
                        break;
                    }
                }
                bool started = false;
                if (start_id != 0) {
                    started = bt_service->start_task(start_id, nullptr);
                }
                if (started) {
                    auto client = bt_service->get_client_by_task_id(start_id);
                    out["ok"] = true;
                    out["message"] = "BitTorrent client started";
                    out["task_id"] = start_id;
                    if (client) {
                        append_bt_task_history(BtTaskHistoryItem{
                            now_ms(),
                            "start",
                            client->get_meta().info_hash_hex_,
                            client->get_meta().info.name_,
                            client->get_save_path(),
                            true,
                        });
                    }
                } else {
                    out["ok"] = false;
                    out["message"] = "no task to start or start failed";
                    resp->json(out.dump(), yuan::net::http::ResponseCode::internal_server_error);
                    resp->send();
                    return;
                }
            } else {
                bool any_stopped = false;
                auto tasks = bt_service->list_tasks();
                for (const auto &t : tasks) {
                    if (t.status == "running") {
                        auto client = bt_service->get_client_by_task_id(t.id);
                        std::string info_hash, name, save_path;
                        if (client) {
                            info_hash = client->get_meta().info_hash_hex_;
                            name = client->get_meta().info.name_;
                            save_path = client->get_save_path();
                        }
                        bt_service->stop_task(t.id);
                        any_stopped = true;
                        append_bt_task_history(BtTaskHistoryItem{
                            now_ms(),
                            "stop",
                            info_hash,
                            name,
                            save_path,
                            false,
                        });
                    }
                }
                out["ok"] = true;
                out["message"] = any_stopped ? "BitTorrent client(s) stopped" : "BitTorrent client already stopped";
            }

            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/torrent", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto torrent_path = input.value("torrent_path", "");
            const auto save_path = input.value("save_path", "");
            if (torrent_path.empty()) {
                resp->json("{\"error\":\"torrent_path_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            std::string task_error;
            const auto task_id = bt_service->add_task(torrent_path, save_path, true, &task_error);
            if (task_id == 0 || !task_error.empty()) {
                nlohmann::json err;
                err["error"] = task_error.empty() ? "add_task_failed" : task_error;
                resp->json(err.dump(), yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto client = bt_service->get_client_by_task_id(task_id);
            if (client) {
                append_bt_task_history(BtTaskHistoryItem{
                    now_ms(),
                    "load",
                    client->get_meta().info_hash_hex_,
                    client->get_meta().info.name_,
                    client->get_save_path(),
                    true,
                });
            }

            nlohmann::json out;
            out["ok"] = true;
            out["task_id"] = task_id;
            out["torrent_path"] = torrent_path;
            out["save_path"] = save_path;
            if (client) {
                out["message"] = "torrent loaded and started";
            } else {
                out["message"] = "task queued (max concurrent downloads reached)";
                out["queued"] = true;
            }
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/magnet", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto magnet_uri = input.value("magnet_uri", "");
            const auto save_path = input.value("save_path", "");
            if (magnet_uri.empty()) {
                resp->json("{\"error\":\"magnet_uri_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            std::string task_error;
            const auto task_id = bt_service->add_magnet_task(magnet_uri, save_path, true, &task_error);
            if (task_id == 0 || !task_error.empty()) {
                nlohmann::json err;
                err["error"] = task_error.empty() ? "add_magnet_task_failed" : task_error;
                resp->json(err.dump(), yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto client = bt_service->get_client_by_task_id(task_id);
            if (client) {
                append_bt_task_history(BtTaskHistoryItem{
                    now_ms(),
                    "magnet",
                    client->get_meta().info_hash_hex_,
                    client->get_meta().info.name_,
                    client->get_save_path(),
                    true,
                });
            }

            nlohmann::json out;
            out["ok"] = true;
            out["task_id"] = task_id;
            out["magnet_uri"] = magnet_uri;
            out["save_path"] = save_path;
            if (client) {
                out["message"] = "magnet link loaded and started";
            } else {
                out["message"] = "task queued (max concurrent downloads reached)";
                out["queued"] = true;
            }
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/upload", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }

            if (!authorize_admin(req, resp)) {
                return;
            }

            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            auto *content = req->get_body_content();
            if (!content || !content->is_valid() || content->type != yuan::net::http::ContentType::multpart_form_data) {
                resp->json("{\"error\":\"multipart_form_data_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto *form = content->as<yuan::net::http::FormDataContent>();
            if (!form) {
                resp->json("{\"error\":\"form_data_parse_failed\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto *file_item = form->get_file("torrent");
            if (!file_item) {
                resp->json("{\"error\":\"torrent_file_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            std::string torrent_data;
            if (file_item->is_in_memory()) {
                if (file_item->data_begin && file_item->data_end > file_item->data_begin) {
                    torrent_data.assign(file_item->data_begin, file_item->data_end);
                }
            } else if (!file_item->tmp_file.empty()) {
                std::ifstream in(file_item->tmp_file, std::ios::binary);
                if (in.good()) {
                    torrent_data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                }
            }

            if (torrent_data.empty()) {
                resp->json("{\"error\":\"torrent_file_empty\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto save_path = form->get_string("save_path");

            std::string task_error;
            const auto task_id = bt_service->add_data_task(torrent_data, save_path, true, &task_error);
            if (task_id == 0 || !task_error.empty()) {
                nlohmann::json err;
                err["error"] = task_error.empty() ? "add_data_task_failed" : task_error;
                resp->json(err.dump(), yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto client = bt_service->get_client_by_task_id(task_id);
            if (client) {
                append_bt_task_history(BtTaskHistoryItem{
                    now_ms(),
                    "upload",
                    client->get_meta().info_hash_hex_,
                    client->get_meta().info.name_,
                    client->get_save_path(),
                    true,
                });
            }

            nlohmann::json out;
            out["ok"] = true;
            out["task_id"] = task_id;
            out["filename"] = file_item->origin_name;
            out["save_path"] = save_path;
            if (client) {
                out["message"] = "torrent uploaded and started";
            } else {
                out["message"] = "task queued (max concurrent downloads reached)";
                out["queued"] = true;
            }
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/tasks", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }
            if (!authorize_admin(req, resp)) {
                return;
            }
            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json items = nlohmann::json::array();
            for (const auto &task : bt_service->list_tasks()) {
                auto client = bt_service->get_client_by_task_id(task.id);
                items.push_back(build_task_stats_json(task, client.get()));
            }

            nlohmann::json out;
            out["active_count"] = bt_service->active_task_count();
            out["max_concurrent"] = bt_service->max_concurrent_downloads();
            out["items"] = std::move(items);
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/tasks/start", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }
            if (!authorize_admin(req, resp)) {
                return;
            }
            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto task_id = input.value("task_id", static_cast<int64_t>(0));
            if (task_id == 0) {
                resp->json("{\"error\":\"task_id_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            std::string err;
            if (!bt_service->start_task(task_id, &err)) {
                nlohmann::json out;
                out["error"] = err.empty() ? "start_task_failed" : err;
                resp->json(out.dump(), yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            nlohmann::json out;
            out["ok"] = true;
            out["task_id"] = task_id;
            out["message"] = "task started";
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/tasks/stop", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }
            if (!authorize_admin(req, resp)) {
                return;
            }
            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto task_id = input.value("task_id", static_cast<int64_t>(0));
            if (task_id == 0) {
                resp->json("{\"error\":\"task_id_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            if (bt_service->stop_task(task_id)) {
                nlohmann::json out;
                out["ok"] = true;
                out["task_id"] = task_id;
                out["message"] = "task stopped";
                resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
                resp->send();
            } else {
                resp->json("{\"error\":\"task_not_active\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
            }
        });

        server_->on("/admin/api/bt/tasks/remove", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::post_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }
            if (!authorize_admin(req, resp)) {
                return;
            }
            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json input;
            try {
                input = nlohmann::json::parse(request_body(req));
            } catch (...) {
                resp->json("{\"error\":\"invalid_json\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            const auto task_id = input.value("task_id", static_cast<int64_t>(0));
            if (task_id == 0) {
                resp->json("{\"error\":\"task_id_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            if (!bt_service->remove_task(task_id)) {
                resp->json("{\"error\":\"task_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            nlohmann::json out;
            out["ok"] = true;
            out["task_id"] = task_id;
            out["message"] = "task removed";
            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });

        server_->on("/admin/api/bt/tasks/detail", [this](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
            if (!req || req->get_method() != yuan::net::http::HttpMethod::get_) {
                resp->json("{\"error\":\"method_not_allowed\"}", yuan::net::http::ResponseCode::method_not_allowed);
                resp->send();
                return;
            }
            if (!authorize_admin(req, resp)) {
                return;
            }
            if (!runtime_context_.service_registry) {
                resp->json("{\"error\":\"service_registry_unavailable\"}", yuan::net::http::ResponseCode::service_unavailable);
                resp->send();
                return;
            }

            auto bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bittorrent");
            if (!bt_service) {
                bt_service = runtime_context_.service_registry->find_service_as<BitTorrentService>("bt");
            }
            if (!bt_service) {
                resp->json("{\"error\":\"bittorrent_service_not_found\"}", yuan::net::http::ResponseCode::not_found);
                resp->send();
                return;
            }

            int64_t task_id = req->get_param_int("task_id", 0);
            if (task_id == 0) {
                resp->json("{\"error\":\"task_id_required\"}", yuan::net::http::ResponseCode::bad_request);
                resp->send();
                return;
            }

            auto client = bt_service->get_client_by_task_id(task_id);
            const bool is_active = (client != nullptr);

            nlohmann::json out;
            out["task_id"] = task_id;
            out["active"] = is_active;
            out["running"] = is_active && client->is_running();
            out["metadata_mode"] = is_active && client->is_metadata_mode();
            out["info_hash"] = is_active ? client->get_meta().info_hash_hex_ : std::string();

            nlohmann::json files = nlohmann::json::array();
            if (is_active && client->has_loaded_torrent()) {
                const auto &meta = client->get_meta();
                const auto &info = meta.info;
                if (info.files_.empty()) {
                    nlohmann::json f;
                    f["name"] = info.name_;
                    f["length"] = info.total_length_;
                    f["offset"] = 0;
                    files.push_back(std::move(f));
                } else {
                    for (const auto &tf : info.files_) {
                        nlohmann::json f;
                        std::string path;
                        for (size_t i = 0; i < tf.path_.size(); ++i) {
                            if (i > 0) path += "/";
                            path += tf.path_[i];
                        }
                        f["name"] = path;
                        f["length"] = tf.length_;
                        f["offset"] = tf.offset_;
                        files.push_back(std::move(f));
                    }
                }
            }
            out["files"] = std::move(files);

            nlohmann::json peers = nlohmann::json::array();
            if (is_active && client->has_loaded_torrent()) {
                const auto active_peers = client->get_active_peers();
                for (const auto &p : active_peers) {
                    if (!p) continue;
                    nlohmann::json pi;
                    pi["ip"] = p->get_peer_ip();
                    pi["port"] = p->get_peer_port();
                    const auto &pid = p->get_peer_id();
                    pi["peer_id"] = yuan::net::bit_torrent::to_hex(
                        reinterpret_cast<const uint8_t *>(pid.data()), pid.size());
                    const auto state = p->get_state();
                    const char *state_str = "unknown";
                    if (state == yuan::net::bit_torrent::PeerConnection::State::idle) state_str = "idle";
                    else if (state == yuan::net::bit_torrent::PeerConnection::State::connecting) state_str = "connecting";
                    else if (state == yuan::net::bit_torrent::PeerConnection::State::handshaking) state_str = "handshaking";
                    else if (state == yuan::net::bit_torrent::PeerConnection::State::connected) state_str = "connected";
                    else if (state == yuan::net::bit_torrent::PeerConnection::State::closed) state_str = "closed";
                    else if (state == yuan::net::bit_torrent::PeerConnection::State::error) state_str = "error";
                    pi["state"] = state_str;
                    const auto &ps = p->get_peer_state();
                    pi["am_choking"] = ps.am_choking;
                    pi["am_interested"] = ps.am_interested;
                    pi["peer_choking"] = ps.peer_choking;
                    pi["peer_interested"] = ps.peer_interested;
                    pi["download_rate"] = static_cast<int64_t>(p->download_rate());
                    pi["upload_rate"] = static_cast<int64_t>(p->upload_rate());
                    pi["snubbed"] = p->is_snubbed();
                    int32_t pieces_have_count = 0;
                    for (bool b : ps.pieces) { if (b) ++pieces_have_count; }
                    pi["pieces_have"] = pieces_have_count;
                    pi["pieces_total"] = static_cast<int32_t>(ps.pieces.size());
                    peers.push_back(std::move(pi));
                }
            }
            out["peers"] = std::move(peers);

            nlohmann::json trackers = nlohmann::json::array();
            if (is_active && client->has_loaded_torrent()) {
                const auto &meta = client->get_meta();
                if (!meta.announce_.empty()) {
                    nlohmann::json t;
                    t["url"] = meta.announce_;
                    t["tier"] = -1;
                    trackers.push_back(std::move(t));
                }
                for (size_t tier = 0; tier < meta.announce_list_.size(); ++tier) {
                    for (const auto &url : meta.announce_list_[tier]) {
                        nlohmann::json t;
                        t["url"] = url;
                        t["tier"] = static_cast<int32_t>(tier);
                        trackers.push_back(std::move(t));
                    }
                }
                const auto &magnet_urls = client->get_magnet_tracker_urls();
                for (const auto &url : magnet_urls) {
                    nlohmann::json t;
                    t["url"] = url;
                    t["tier"] = -2;
                    t["source"] = "magnet";
                    trackers.push_back(std::move(t));
                }
            }
            out["trackers"] = std::move(trackers);

            nlohmann::json pieces;
            if (is_active && client->has_loaded_torrent()) {
                const auto &have = client->get_pieces_have();
                const auto &downloading = client->get_pieces_downloading();
                int32_t done = 0;
                int32_t active = 0;
                int32_t pending = 0;
                const int32_t total = static_cast<int32_t>(have.size());
                for (int32_t i = 0; i < total; ++i) {
                    if (have[i]) ++done;
                    else if (downloading[i]) ++active;
                    else ++pending;
                }
                pieces["total"] = total;
                pieces["done"] = done;
                pieces["active"] = active;
                pieces["pending"] = pending;
                pieces["remaining"] = client->remaining_piece_count();
                pieces["endgame"] = (total > 0 && (total - done) <= 8);
            } else {
                pieces["total"] = 0;
                pieces["done"] = 0;
                pieces["active"] = 0;
                pieces["pending"] = 0;
                pieces["remaining"] = 0;
                pieces["endgame"] = false;
            }
            out["pieces"] = std::move(pieces);

            resp->json(out.dump(), yuan::net::http::ResponseCode::ok_);
            resp->send();
        });
    }

    void HttpService::subscribe_dashboard_events()
    {
        unsubscribe_dashboard_events();
        if (!runtime_context_.event_bus) {
            return;
        }

        auto add_counter_subscription = [this](const std::string &event_name) {
            auto token = runtime_context_.event_bus->subscribe(event_name, [this, event_name](const yuan::eventbus::Event &) {
                const auto ts = now_ms();
                std::lock_guard<std::mutex> lock(dashboard_mutex_);
                ++event_counters_[event_name];
                dashboard_snapshot_.last_event_ms = ts;
                recent_events_.push_back(DashboardEvent{ts, event_name, {}});
                while (recent_events_.size() > kDashboardRecentEventLimit) {
                    recent_events_.pop_front();
                }
            });
            if (token != 0) {
                event_tokens_.push_back(token);
            }
        };

        const std::vector<std::string> subscribed = {
            yuan::app::events::application_started,
            yuan::app::events::application_stopping,
            yuan::app::events::service_initialized,
            yuan::app::events::service_started,
            yuan::app::events::service_stopped,
            yuan::server::events::service_activating,
            yuan::server::events::service_activated,
            yuan::server::events::service_stopping,
            yuan::server::events::service_stopped,
            yuan::server::events::bittorrent_peer_connected,
            yuan::server::events::bittorrent_piece_completed,
            yuan::server::events::bittorrent_torrent_completed,
            yuan::server::events::bittorrent_metadata_received,
        };

        for (const auto &event_name : subscribed) {
            add_counter_subscription(event_name);
        }

        auto service_lifecycle_handler = [this](bool started, const yuan::eventbus::Event &event) {
            if (const auto *payload = std::any_cast<yuan::server::ServiceRuntimeEvent>(&event.payload)) {
                std::lock_guard<std::mutex> lock(dashboard_mutex_);
                service_states_[payload->service_name] = started;
            }
        };

        auto token_activated = runtime_context_.event_bus->subscribe(yuan::server::events::service_activated,
            [service_lifecycle_handler](const yuan::eventbus::Event &event) {
                service_lifecycle_handler(true, event);
            });
        if (token_activated != 0) {
            event_tokens_.push_back(token_activated);
        }

        auto token_stopped = runtime_context_.event_bus->subscribe(yuan::server::events::service_stopped,
            [service_lifecycle_handler](const yuan::eventbus::Event &event) {
                service_lifecycle_handler(false, event);
            });
        if (token_stopped != 0) {
            event_tokens_.push_back(token_stopped);
        }

        auto token_bt_peer = runtime_context_.event_bus->subscribe(yuan::server::events::bittorrent_peer_connected,
            [this](const yuan::eventbus::Event &event) {
                if (const auto *payload = std::any_cast<yuan::server::BitTorrentPeerEvent>(&event.payload)) {
                    std::lock_guard<std::mutex> lock(dashboard_mutex_);
                    ++dashboard_snapshot_.bt_peer_connected_total;
                    dashboard_snapshot_.bt_last_info_hash = payload->info_hash;
                    recent_events_.push_back(DashboardEvent{now_ms(), event.name, payload->peer_ip + ":" + std::to_string(payload->peer_port)});
                    while (recent_events_.size() > kDashboardRecentEventLimit) {
                        recent_events_.pop_front();
                    }
                }
            });
        if (token_bt_peer != 0) {
            event_tokens_.push_back(token_bt_peer);
        }

        auto token_bt_piece = runtime_context_.event_bus->subscribe(yuan::server::events::bittorrent_piece_completed,
            [this](const yuan::eventbus::Event &event) {
                if (const auto *payload = std::any_cast<yuan::server::BitTorrentPieceEvent>(&event.payload)) {
                    std::lock_guard<std::mutex> lock(dashboard_mutex_);
                    ++dashboard_snapshot_.bt_piece_completed_total;
                    dashboard_snapshot_.bt_piece_completed_bytes += payload->piece_size;
                    dashboard_snapshot_.bt_last_info_hash = payload->info_hash;
                    recent_events_.push_back(DashboardEvent{now_ms(), event.name, "piece=" + std::to_string(payload->piece_index)});
                    while (recent_events_.size() > kDashboardRecentEventLimit) {
                        recent_events_.pop_front();
                    }
                }
            });
        if (token_bt_piece != 0) {
            event_tokens_.push_back(token_bt_piece);
        }

        auto token_bt_done = runtime_context_.event_bus->subscribe(yuan::server::events::bittorrent_torrent_completed,
            [this](const yuan::eventbus::Event &event) {
                if (const auto *payload = std::any_cast<yuan::server::BitTorrentTorrentEvent>(&event.payload)) {
                    std::lock_guard<std::mutex> lock(dashboard_mutex_);
                    ++dashboard_snapshot_.bt_torrent_completed_total;
                    dashboard_snapshot_.bt_last_info_hash = payload->info_hash;
                    dashboard_snapshot_.bt_last_torrent_name = payload->name;
                    recent_events_.push_back(DashboardEvent{now_ms(), event.name, payload->name});
                    while (recent_events_.size() > kDashboardRecentEventLimit) {
                        recent_events_.pop_front();
                    }
                }
            });
        if (token_bt_done != 0) {
            event_tokens_.push_back(token_bt_done);
        }

    }

    void HttpService::unsubscribe_dashboard_events()
    {
        if (!runtime_context_.event_bus) {
            event_tokens_.clear();
            return;
        }

        for (auto token : event_tokens_) {
            if (token != 0) {
                runtime_context_.event_bus->unsubscribe(token);
            }
        }
        event_tokens_.clear();
    }

    bool HttpService::authorize_admin(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) const
    {
        (void)req;
        const char *token_env = std::getenv("YUAN_ADMIN_TOKEN");
        if (!token_env || std::string(token_env).empty()) {
            return true;
        }

        const std::string expected = token_env;
        const std::string auth = request_header(req, "authorization");
        const std::string expected_header = "Bearer " + expected;
        if (auth == expected_header) {
            return true;
        }

        nlohmann::json out;
        out["error"] = "unauthorized";
        out["hint"] = "use Authorization: Bearer <YUAN_ADMIN_TOKEN>";
        resp->json(out.dump(), yuan::net::http::ResponseCode::unauthorized);
        resp->send();
        return false;
    }

} // namespace yuan::server
