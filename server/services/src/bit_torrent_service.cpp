#include "bit_torrent_service.h"

#include "server_service_custom_events.h"
#include "utils.h"
#include "magnet_uri.h"
#include "nat/nat_manager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

namespace yuan::server
{
    namespace
    {
        std::string tasks_file_path()
        {
            return "bt_downloader_tasks.json";
        }
    }

    BitTorrentService::BitTorrentService(std::string torrent_file_path, std::string save_path)
        : torrent_file_path_(std::move(torrent_file_path)), save_path_(std::move(save_path)), host_({ "bit_torrent", "bittorrent", 0 })
    {
    }

    BitTorrentService::~BitTorrentService()
    {
        stop();
    }

    void BitTorrentService::setup_client_callbacks(yuan::net::bit_torrent::BitTorrentClient &client, int64_t task_id)
    {
        client.set_peer_connected_callback([this, task_id](const std::string &peer_ip, uint16_t peer_port, const std::string &peer_id) {
            BitTorrentPeerEvent evt;
            evt.service_name = "bittorrent";
            evt.peer_id = yuan::net::bit_torrent::to_hex(reinterpret_cast<const uint8_t *>(peer_id.data()), peer_id.size());
            evt.peer_ip = peer_ip;
            evt.peer_port = peer_port;
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto it = active_clients_.find(task_id);
                if (it != active_clients_.end()) {
                    evt.info_hash = it->second->get_meta().info_hash_hex_;
                }
            }
            host_.publish_custom(events::bittorrent_peer_connected, std::move(evt));
        });

        client.set_piece_completed_callback([this, task_id](uint32_t piece_index, uint32_t piece_size) {
            BitTorrentPieceEvent evt;
            evt.service_name = "bittorrent";
            evt.piece_index = static_cast<int>(piece_index);
            evt.piece_size = piece_size;
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto it = active_clients_.find(task_id);
                if (it != active_clients_.end()) {
                    evt.info_hash = it->second->get_meta().info_hash_hex_;
                }
            }
            host_.publish_custom(events::bittorrent_piece_completed, std::move(evt));
        });

        client.set_torrent_completed_callback([this, task_id]() {
            BitTorrentTorrentEvent evt;
            evt.service_name = "bittorrent";
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto it = active_clients_.find(task_id);
                if (it == active_clients_.end()) {
                    return;
                }
                auto &cl = it->second;
                const auto &meta = cl->get_meta();
                evt.info_hash = meta.info_hash_hex_;
                evt.name = meta.info.name_;
                evt.total_size = static_cast<std::size_t>(meta.info.total_length_);

                if (auto *task = find_task_nolock(task_id)) {
                    task->status = "seeding";
                    task->updated_at_ms = now_ms();
                    task->last_error.clear();
                }
                persist_tasks_to_disk_nolock();

                seeding_clients_[task_id] = std::move(cl);
                active_clients_.erase(it);
                try_start_queued_tasks_nolock();
            }
            host_.publish_custom(events::bittorrent_torrent_completed, std::move(evt));
        });

         client.set_metadata_received_callback([this, task_id]() {
             BitTorrentTorrentEvent evt;
             evt.service_name = "bittorrent";
             std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient> client_ref;
             {
                 std::lock_guard<std::mutex> lock(tasks_mutex_);
                 auto it = active_clients_.find(task_id);
                 if (it == active_clients_.end()) {
                     return;
                 }
                 client_ref = it->second;
                 const auto &meta = client_ref->get_meta();
                 evt.info_hash = meta.info_hash_hex_;
                 evt.name = meta.info.name_;
                 evt.total_size = static_cast<std::size_t>(meta.info.total_length_);

                 if (auto *task = find_task_nolock(task_id)) {
                     task->name = meta.info.name_;
                     task->total_size = meta.info.total_length_;
                     task->updated_at_ms = now_ms();
                     persist_tasks_to_disk_nolock();
                 }
             }

             host_.publish_custom(events::bittorrent_metadata_received, std::move(evt));
             if (client_ref) {
                 setup_shared_dht_for_client(*client_ref);
             }
         });
    }

    bool BitTorrentService::init()
    {
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            load_tasks_from_disk_nolock();
        }

        return true;
    }

    uint64_t BitTorrentService::now_ms()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    BitTorrentService::ManagedTask *BitTorrentService::find_task_nolock(int64_t task_id)
    {
        for (auto &task : tasks_) {
            if (task.id == task_id) {
                return &task;
            }
        }
        return nullptr;
    }

    const BitTorrentService::ManagedTask *BitTorrentService::find_task_nolock(int64_t task_id) const
    {
        for (const auto &task : tasks_) {
            if (task.id == task_id) {
                return &task;
            }
        }
        return nullptr;
    }

    bool BitTorrentService::load_task_into_client_nolock(ManagedTask &task, yuan::net::bit_torrent::BitTorrentClient &client, std::string *error)
    {
        client.stop();
        const std::string effective_save = task.save_path.empty() ? save_path_ : task.save_path;
        client.set_save_path(effective_save);

        bool loaded = false;
        if (!task.magnet_uri.empty()) {
            loaded = client.load_magnet(task.magnet_uri);
            if (!loaded) {
                if (error) {
                    *error = "load_magnet_failed";
                }
                task.status = "error";
                task.last_error = "load_magnet_failed";
                task.updated_at_ms = now_ms();
                persist_tasks_to_disk_nolock();
                return false;
            }
            const auto &meta = client.get_meta();
            task.info_hash = meta.info_hash_hex_;
            if (task.name.empty() && !meta.info.name_.empty()) {
                task.name = meta.info.name_;
            }
            task.save_path = effective_save;
            task.last_error.clear();
            task.status = "loaded";
            task.updated_at_ms = now_ms();
            persist_tasks_to_disk_nolock();
            return true;
        }

        if (!task.torrent_path.empty()) {
            loaded = client.load_torrent(task.torrent_path);
        }

        if (!loaded) {
            if (error) {
                *error = "load_torrent_failed";
            }
            task.status = "error";
            task.last_error = "load_torrent_failed";
            task.updated_at_ms = now_ms();
            persist_tasks_to_disk_nolock();
            return false;
        }

        const auto &meta = client.get_meta();
        task.info_hash = meta.info_hash_hex_;
        task.name = meta.info.name_;
        task.total_size = meta.info.total_length_;
        task.save_path = effective_save;
        task.last_error.clear();
        task.status = "loaded";
        task.updated_at_ms = now_ms();
        persist_tasks_to_disk_nolock();
        return true;
    }

    void BitTorrentService::try_start_queued_tasks_nolock()
    {
        while (static_cast<int32_t>(active_clients_.size()) < max_concurrent_downloads_) {
            ManagedTask *next = nullptr;
            for (auto &task : tasks_) {
                if (task.status == "queued") {
                    next = &task;
                    break;
                }
            }
            if (!next) {
                break;
            }

            auto client = std::make_shared<yuan::net::bit_torrent::BitTorrentClient>();
            setup_client_callbacks(*client, next->id);
            apply_default_settings(*client);

            if (shared_runtime_) {
                client->set_runtime(*shared_runtime_);
            }

            if (!load_task_into_client_nolock(*next, *client, nullptr)) {
                release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                continue;
            }

            if (!client->start()) {
                release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                next->status = "error";
                next->last_error = "start_task_failed";
                next->updated_at_ms = now_ms();
                persist_tasks_to_disk_nolock();
                continue;
            }

            next->status = "running";
            next->updated_at_ms = now_ms();
            next->last_error.clear();
            add_nat_port_mapping_for_client(*client);
            setup_shared_dht_for_client(*client);
            active_clients_[next->id] = std::move(client);
            persist_tasks_to_disk_nolock();
        }
    }

    void BitTorrentService::apply_default_settings(yuan::net::bit_torrent::BitTorrentClient &client)
    {
        client.set_max_peers(default_max_peers_);
        client.set_listen_port(allocate_listen_port());
        client.set_download_limit_kbps(default_download_limit_kbps_);
        client.set_upload_limit_kbps(default_upload_limit_kbps_);

        auto nat_cfg = default_nat_config_;
        if (bt_runtime_ || shared_runtime_) {
            nat_cfg.enable_upnp = false;
            nat_cfg.enable_nat_pmp = false;
        }
        if (shared_dht_node_) {
            nat_cfg.enable_dht = false;
        }
        client.set_nat_config(nat_cfg);
    }

    void BitTorrentService::add_nat_port_mapping_for_client(yuan::net::bit_torrent::BitTorrentClient &client)
    {
        if (!nat_manager_ || !nat_manager_->is_igd_discovered()) {
            return;
        }
        uint16_t port = static_cast<uint16_t>(client.get_listen_port());
        auto *nat = nat_manager_.get();
        std::thread t([nat, port]() {
            nat->add_port_mapping(port);
        });
        t.detach();
    }

    void BitTorrentService::remove_nat_port_mapping_for_client(yuan::net::bit_torrent::BitTorrentClient &client)
    {
        if (!nat_manager_ || !nat_manager_->is_igd_discovered()) {
            return;
        }
        uint16_t port = static_cast<uint16_t>(client.get_listen_port());
        auto *nat = nat_manager_.get();
        std::thread t([nat, port]() {
            nat->remove_port_mapping(port);
        });
        t.detach();
    }

    void BitTorrentService::setup_shared_dht_for_client(yuan::net::bit_torrent::BitTorrentClient &client)
    {
        if (!shared_dht_node_ || !shared_dht_node_->is_running()) {
            return;
        }
        const auto &meta = client.get_meta();
        if (meta.info_hash_.empty() || meta.info.private_ || client.is_metadata_mode()) {
            return;
        }
        uint16_t port = static_cast<uint16_t>(client.get_listen_port());
        auto *nat = client.get_nat_manager();
        shared_dht_node_->announce(meta.info_hash_, port,
            [nat](const std::vector<yuan::net::bit_torrent::PeerAddress> &peers) {
                if (nat) {
                    nat->on_dht_peers(peers);
                }
            });
    }

    uint16_t BitTorrentService::allocate_listen_port()
    {
        for (int32_t port = next_listen_port_; port <= default_listen_port_end_; ++port) {
            if (used_ports_.find(static_cast<uint16_t>(port)) == used_ports_.end()) {
                used_ports_.insert(static_cast<uint16_t>(port));
                next_listen_port_ = port + 1;
                if (next_listen_port_ > default_listen_port_end_) {
                    next_listen_port_ = default_listen_port_;
                }
                return static_cast<uint16_t>(port);
            }
        }
        next_listen_port_ = default_listen_port_;
        for (int32_t port = default_listen_port_; port <= default_listen_port_end_; ++port) {
            if (used_ports_.find(static_cast<uint16_t>(port)) == used_ports_.end()) {
                used_ports_.insert(static_cast<uint16_t>(port));
                next_listen_port_ = port + 1;
                return static_cast<uint16_t>(port);
            }
        }
        return static_cast<uint16_t>(default_listen_port_);
    }

    void BitTorrentService::release_listen_port(uint16_t port)
    {
        used_ports_.erase(port);
    }

    int64_t BitTorrentService::add_task(const std::string &torrent_path, const std::string &save_path, bool auto_start, std::string *error)
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);

        ManagedTask task;
        task.id = next_task_id_++;
        task.torrent_path = torrent_path;
        task.save_path = save_path.empty() ? save_path_ : save_path;
        task.status = "queued";
        task.updated_at_ms = now_ms();

        tasks_.push_back(task);
        persist_tasks_to_disk_nolock();
        auto *created = find_task_nolock(task.id);
        if (!created) {
            if (error) {
                *error = "create_task_failed";
            }
            return 0;
        }

        if (auto_start) {
            if (static_cast<int32_t>(active_clients_.size()) < max_concurrent_downloads_) {
                auto client = std::make_shared<yuan::net::bit_torrent::BitTorrentClient>();
                setup_client_callbacks(*client, created->id);
                apply_default_settings(*client);
                if (shared_runtime_) {
                    client->set_runtime(*shared_runtime_);
                }

                if (!load_task_into_client_nolock(*created, *client, error)) {
                    release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                    return created->id;
                }
                if (!client->start()) {
                    release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                    if (error) {
                        *error = "start_task_failed";
                    }
                    created->status = "error";
                    created->last_error = "start_task_failed";
                    created->updated_at_ms = now_ms();
                    persist_tasks_to_disk_nolock();
                    return created->id;
                }
                created->status = "running";
                created->updated_at_ms = now_ms();
                add_nat_port_mapping_for_client(*client);
                setup_shared_dht_for_client(*client);
                active_clients_[created->id] = std::move(client);
                persist_tasks_to_disk_nolock();
            }
        }

        return created->id;
    }

    int64_t BitTorrentService::add_data_task(const std::string &torrent_data, const std::string &save_path, bool auto_start, std::string *error)
    {
        if (torrent_data.empty()) {
            if (error) *error = "torrent_data_empty";
            return 0;
        }

        auto meta = yuan::net::bit_torrent::TorrentMeta::parse(torrent_data);
        if (meta.info_hash_.empty()) {
            if (error) *error = "invalid_torrent_data";
            return 0;
        }

        std::string torrent_dir = "torrents";
        std::error_code ec;
        std::filesystem::create_directories(torrent_dir, ec);

        std::string torrent_path = torrent_dir + "/" + meta.info_hash_hex_ + ".torrent";
        {
            std::ofstream out(torrent_path, std::ios::binary | std::ios::trunc);
            if (!out.good()) {
                if (error) *error = "save_torrent_failed";
                return 0;
            }
            out.write(torrent_data.data(), static_cast<std::streamsize>(torrent_data.size()));
        }

        auto task_id = add_task(torrent_path, save_path, auto_start, error);
        if (task_id != 0) {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            if (auto *task = find_task_nolock(task_id)) {
                task->has_torrent_data = true;
            }
        }
        return task_id;
    }

    int64_t BitTorrentService::add_magnet_task(const std::string &magnet_uri, const std::string &save_path, bool auto_start, std::string *error)
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);

        ManagedTask task;
        task.id = next_task_id_++;
        task.magnet_uri = magnet_uri;
        task.save_path = save_path.empty() ? save_path_ : save_path;
        task.status = "queued";
        task.updated_at_ms = now_ms();

        auto parsed = yuan::net::bit_torrent::MagnetUri::parse(magnet_uri);
        if (parsed.valid) {
            task.info_hash = parsed.info_hash_hex;
            if (!parsed.display_name.empty()) {
                task.name = parsed.display_name;
            }
        }

        tasks_.push_back(task);
        persist_tasks_to_disk_nolock();
        auto *created = find_task_nolock(task.id);
        if (!created) {
            if (error) {
                *error = "create_task_failed";
            }
            return 0;
        }

        if (auto_start) {
            if (static_cast<int32_t>(active_clients_.size()) < max_concurrent_downloads_) {
                auto client = std::make_shared<yuan::net::bit_torrent::BitTorrentClient>();
                setup_client_callbacks(*client, created->id);
                apply_default_settings(*client);
                if (shared_runtime_) {
                    client->set_runtime(*shared_runtime_);
                }

                if (!load_task_into_client_nolock(*created, *client, error)) {
                    release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                    return created->id;
                }
                if (!client->start()) {
                    release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
                    if (error) {
                        *error = "start_task_failed";
                    }
                    created->status = "error";
                    created->last_error = "start_task_failed";
                    created->updated_at_ms = now_ms();
                    persist_tasks_to_disk_nolock();
                    return created->id;
                }
                created->status = "running";
                created->updated_at_ms = now_ms();
                add_nat_port_mapping_for_client(*client);
                setup_shared_dht_for_client(*client);
                active_clients_[created->id] = std::move(client);
                persist_tasks_to_disk_nolock();
            }
        }

        return created->id;
    }

    bool BitTorrentService::start_task(int64_t task_id, std::string *error)
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto *task = find_task_nolock(task_id);
        if (!task) {
            if (error) {
                *error = "task_not_found";
            }
            return false;
        }

        if (task->status == "running" || task->status == "seeding") {
            if (error) {
                *error = "task_already_running";
            }
            return true;
        }

        if (static_cast<int32_t>(active_clients_.size()) >= max_concurrent_downloads_) {
            if (error) {
                *error = "max_concurrent_reached (" + std::to_string(active_clients_.size()) + "/" + std::to_string(max_concurrent_downloads_) + ")";
            }
            return false;
        }

        auto client = std::make_shared<yuan::net::bit_torrent::BitTorrentClient>();
        setup_client_callbacks(*client, task_id);
        apply_default_settings(*client);
        if (shared_runtime_) {
            client->set_runtime(*shared_runtime_);
        }

        if (!load_task_into_client_nolock(*task, *client, error)) {
            release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
            return false;
        }

        if (!client->start()) {
            release_listen_port(static_cast<uint16_t>(client->get_listen_port()));
            if (error) {
                *error = "start_task_failed";
            }
            task->status = "error";
            task->last_error = "start_task_failed";
            task->updated_at_ms = now_ms();
            persist_tasks_to_disk_nolock();
            return false;
        }

        task->status = "running";
        task->updated_at_ms = now_ms();
        task->last_error.clear();
        add_nat_port_mapping_for_client(*client);
        setup_shared_dht_for_client(*client);
        active_clients_[task_id] = std::move(client);
        persist_tasks_to_disk_nolock();
        return true;
    }

    bool BitTorrentService::stop_task(int64_t task_id)
    {
        std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient> client_to_stop;
        uint16_t port = 0;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            auto it = active_clients_.find(task_id);
            if (it == active_clients_.end()) {
                auto sit = seeding_clients_.find(task_id);
                if (sit != seeding_clients_.end()) {
                    remove_nat_port_mapping_for_client(*sit->second);
                    port = static_cast<uint16_t>(sit->second->get_listen_port());
                    client_to_stop = std::move(sit->second);
                    seeding_clients_.erase(sit);
                    if (auto *task = find_task_nolock(task_id)) {
                        task->status = "completed";
                        task->updated_at_ms = now_ms();
                    }
                    persist_tasks_to_disk_nolock();
                } else {
                    auto *task = find_task_nolock(task_id);
                    if (task && task->status == "running") {
                        task->status = "stopped";
                        task->updated_at_ms = now_ms();
                        persist_tasks_to_disk_nolock();
                    }
                    return false;
                }
            } else {
                remove_nat_port_mapping_for_client(*it->second);
                port = static_cast<uint16_t>(it->second->get_listen_port());
                client_to_stop = std::move(it->second);
                active_clients_.erase(it);

                if (auto *task = find_task_nolock(task_id)) {
                    task->status = "stopped";
                    task->updated_at_ms = now_ms();
                }

                try_start_queued_tasks_nolock();
                persist_tasks_to_disk_nolock();
            }
        }
        release_listen_port(port);
        if (client_to_stop) {
            std::thread t([client = std::move(client_to_stop)]() {
                client->stop();
                client->clear_torrent();
            });
            t.detach();
        }
        return true;
    }

    bool BitTorrentService::remove_task(int64_t task_id)
    {
        std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient> client_to_stop;
        uint16_t port = 0;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);

            auto it = active_clients_.find(task_id);
            if (it != active_clients_.end()) {
                remove_nat_port_mapping_for_client(*it->second);
                port = static_cast<uint16_t>(it->second->get_listen_port());
                client_to_stop = std::move(it->second);
                active_clients_.erase(it);
            }

            auto sit = seeding_clients_.find(task_id);
            if (sit != seeding_clients_.end()) {
                if (port == 0) {
                    remove_nat_port_mapping_for_client(*sit->second);
                    port = static_cast<uint16_t>(sit->second->get_listen_port());
                }
                client_to_stop = std::move(sit->second);
                seeding_clients_.erase(sit);
            }

            const auto rit = std::remove_if(tasks_.begin(), tasks_.end(), [task_id](const ManagedTask &task) {
                return task.id == task_id;
            });
            if (rit == tasks_.end()) {
                return false;
            }
            tasks_.erase(rit, tasks_.end());

            try_start_queued_tasks_nolock();
            persist_tasks_to_disk_nolock();
        }
        release_listen_port(port);
        if (client_to_stop) {
            std::thread t([client = std::move(client_to_stop)]() {
                client->stop();
                client->clear_torrent();
            });
            t.detach();
        }
        return true;
    }

    void BitTorrentService::load_tasks_from_disk_nolock()
    {
        std::ifstream in(tasks_file_path(), std::ios::binary);
        if (!in.good()) {
            return;
        }

        nlohmann::json root;
        try {
            in >> root;
        } catch (...) {
            return;
        }

        if (!root.is_object()) {
            return;
        }

        tasks_.clear();
        next_task_id_ = root.value("next_task_id", static_cast<int64_t>(1));

        const auto arr = root.value("tasks", nlohmann::json::array());
        if (!arr.is_array()) {
            return;
        }

        for (const auto &j : arr) {
            ManagedTask task;
            task.id = j.value("id", static_cast<int64_t>(0));
            task.torrent_path = j.value("torrent_path", std::string());
            task.magnet_uri = j.value("magnet_uri", std::string());
            task.save_path = j.value("save_path", std::string());
            task.info_hash = j.value("info_hash", std::string());
            task.name = j.value("name", std::string());
            task.total_size = j.value("total_size", static_cast<int64_t>(0));
            task.status = j.value("status", std::string("queued"));
            task.last_error = j.value("last_error", std::string());
            task.updated_at_ms = j.value("updated_at_ms", static_cast<uint64_t>(0));
            task.has_torrent_data = j.value("has_torrent_data", false);
            if (task.id > 0) {
                tasks_.push_back(std::move(task));
            }
        }

        for (auto &task : tasks_) {
            if (task.status == "running" || task.status == "loaded" || task.status == "seeding") {
                task.status = task.status == "seeding" ? "completed" : "stopped";
                task.updated_at_ms = now_ms();
            }
        }
    }

    void BitTorrentService::persist_tasks_to_disk_nolock() const
    {
        nlohmann::json root;
        root["next_task_id"] = next_task_id_;

        nlohmann::json arr = nlohmann::json::array();
        for (const auto &task : tasks_) {
            nlohmann::json j;
            j["id"] = task.id;
            j["torrent_path"] = task.torrent_path;
            j["magnet_uri"] = task.magnet_uri;
            j["save_path"] = task.save_path;
            j["info_hash"] = task.info_hash;
            j["name"] = task.name;
            j["total_size"] = task.total_size;
            j["status"] = task.status;
            j["last_error"] = task.last_error;
            j["updated_at_ms"] = task.updated_at_ms;
            j["has_torrent_data"] = task.has_torrent_data;
            arr.push_back(std::move(j));
        }
        root["tasks"] = std::move(arr);

        std::ofstream out(tasks_file_path(), std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return;
        }
        out << root.dump(2);
    }

    std::vector<BitTorrentService::ManagedTask> BitTorrentService::list_tasks() const
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        return tasks_;
    }

    int32_t BitTorrentService::active_task_count() const
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        return static_cast<int32_t>(active_clients_.size());
    }

    int32_t BitTorrentService::max_concurrent_downloads() const
    {
        return max_concurrent_downloads_;
    }

    void BitTorrentService::set_max_concurrent_downloads(int32_t max)
    {
        if (max < 1) max = 1;
        max_concurrent_downloads_ = max;
    }

    void BitTorrentService::try_start_queued_tasks()
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        try_start_queued_tasks_nolock();
    }

    std::unordered_map<int64_t, std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient>> BitTorrentService::active_clients() const
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        return active_clients_;
    }

    std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient> BitTorrentService::get_client_by_task_id(int64_t task_id)
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_clients_.find(task_id);
        if (it != active_clients_.end()) return it->second;
        auto sit = seeding_clients_.find(task_id);
        return sit != seeding_clients_.end() ? sit->second : nullptr;
    }

    std::shared_ptr<const yuan::net::bit_torrent::BitTorrentClient> BitTorrentService::get_client_by_task_id(int64_t task_id) const
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = active_clients_.find(task_id);
        if (it != active_clients_.end()) return it->second;
        auto sit = seeding_clients_.find(task_id);
        return sit != seeding_clients_.end() ? sit->second : nullptr;
    }

    void BitTorrentService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        host_.set_runtime_context(context);
        if (context.shared_runtime) {
            shared_runtime_ = context.shared_runtime;
        } else {
            bt_runtime_ = std::make_unique<yuan::net::NetworkRuntime>();
            shared_runtime_ = bt_runtime_.get();
        }
    }

    void BitTorrentService::start()
    {
        if (bt_runtime_) {
            bt_runtime_thread_ = std::thread([this]() {
                bt_runtime_->run();
            });
        }

        if (default_nat_config_.enable_upnp || default_nat_config_.enable_nat_pmp) {
            nat_manager_ = std::make_unique<yuan::net::bit_torrent::UpnpManager>();
            nat_manager_->start(default_nat_config_, static_cast<uint16_t>(default_listen_port_),
                                [this](bool success, const std::string &external_ip, uint16_t mapped_port) {
                                    if (success) {
                                        std::cout << "NAT: mapped " << mapped_port << " -> " << external_ip << std::endl;
                                    } else {
                                        std::cout << "NAT: mapping failed" << std::endl;
                                    }
                                });
        }

        if (default_nat_config_.enable_dht && shared_runtime_) {
            shared_dht_node_ = std::make_unique<yuan::net::bit_torrent::DhtNode>();
            shared_runtime_->dispatch([this]() {
                std::string ext_ip;
                if (nat_manager_ && nat_manager_->is_igd_discovered()) {
                    ext_ip = nat_manager_->get_external_ip();
                }
                if (!shared_dht_node_->start(default_nat_config_, shared_runtime_, ext_ip)) {
                    std::cout << "DHT: shared node start failed" << std::endl;
                    shared_dht_node_.reset();
                } else {
                    std::cout << "DHT: shared node started on port " << shared_dht_node_->get_port() << std::endl;
                }
            });
        }

        host_.start([this]() {
            if (!torrent_file_path_.empty() && tasks_.empty()) {
                std::string error;
                (void)add_task(torrent_file_path_, save_path_, true, &error);
            } else {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                try_start_queued_tasks_nolock();
            }
        });
    }

    void BitTorrentService::stop()
    {
        host_.stop([this]() {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            std::vector<uint16_t> nat_ports;
            for (auto &pair : active_clients_) {
                if (pair.second && nat_manager_ && nat_manager_->is_igd_discovered()) {
                    nat_ports.push_back(static_cast<uint16_t>(pair.second->get_listen_port()));
                }
            }
            for (auto &pair : seeding_clients_) {
                if (pair.second && nat_manager_ && nat_manager_->is_igd_discovered()) {
                    nat_ports.push_back(static_cast<uint16_t>(pair.second->get_listen_port()));
                }
            }
            for (auto &pair : active_clients_) {
                release_listen_port(static_cast<uint16_t>(pair.second->get_listen_port()));
                pair.second->stop();
            }
            active_clients_.clear();
            for (auto &pair : seeding_clients_) {
                release_listen_port(static_cast<uint16_t>(pair.second->get_listen_port()));
                pair.second->stop();
            }
            seeding_clients_.clear();
            for (auto &task : tasks_) {
                if (task.status == "running" || task.status == "seeding") {
                    task.status = "stopped";
                    task.updated_at_ms = now_ms();
                }
            }
            used_ports_.clear();
            persist_tasks_to_disk_nolock();
            if (!nat_ports.empty()) {
                auto *nat = nat_manager_.get();
                std::thread t([nat, ports = std::move(nat_ports)]() {
                    for (auto port : ports) {
                        nat->remove_port_mapping(port);
                    }
                });
                t.detach();
            }
        });

        if (bt_runtime_) {
            bt_runtime_->stop();
        }
        if (bt_runtime_thread_.joinable()) {
            bt_runtime_thread_.join();
        }

        if (nat_manager_) {
            nat_manager_->stop();
            nat_manager_.reset();
        }

        if (shared_dht_node_) {
            if (bt_runtime_) {
                shared_dht_node_->stop();
                shared_dht_node_.reset();
            } else if (shared_runtime_) {
                auto shared_dht = std::shared_ptr<yuan::net::bit_torrent::DhtNode>(shared_dht_node_.release());
                shared_runtime_->dispatch([shared_dht]() mutable {
                    shared_dht->stop();
                });
            }
        }
    }

} // namespace yuan::server
