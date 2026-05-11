#ifndef __SERVER_BIT_TORRENT_SERVICE_H__
#define __SERVER_BIT_TORRENT_SERVICE_H__

#include "bit_torrent_client.h"
#include "nat/upnp_manager.h"
#include "nat/dht_node.h"
#include "server_runtime_host.h"
#include "service.h"
#include "net/runtime/network_runtime.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace yuan::server
{

    class BitTorrentService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        struct ManagedTask
        {
            int64_t id = 0;
            std::string torrent_path;
            std::string magnet_uri;
            std::string save_path;
            std::string info_hash;
            std::string name;
            int64_t total_size = 0;
            std::string status;
            std::string last_error;
            uint64_t updated_at_ms = 0;
            bool has_torrent_data = false;
        };

        explicit BitTorrentService(std::string torrent_file_path, std::string save_path = ".");
        ~BitTorrentService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient> get_client_by_task_id(int64_t task_id);
        std::shared_ptr<const yuan::net::bit_torrent::BitTorrentClient> get_client_by_task_id(int64_t task_id) const;

        int64_t add_task(const std::string &torrent_path, const std::string &save_path, bool auto_start, std::string *error = nullptr);
        int64_t add_data_task(const std::string &torrent_data, const std::string &save_path, bool auto_start, std::string *error = nullptr);
        int64_t add_magnet_task(const std::string &magnet_uri, const std::string &save_path, bool auto_start, std::string *error = nullptr);
        bool start_task(int64_t task_id, std::string *error = nullptr);
        bool stop_task(int64_t task_id);
        bool pause_task(int64_t task_id, std::string *error = nullptr);
        bool resume_task(int64_t task_id, std::string *error = nullptr);
        bool remove_task(int64_t task_id);
        std::vector<ManagedTask> list_tasks() const;
        int32_t active_task_count() const;
        int32_t max_concurrent_downloads() const;
        void set_max_concurrent_downloads(int32_t max);
        void try_start_queued_tasks();
        void set_default_max_peers(int32_t v) { default_max_peers_ = v; }
        int32_t default_max_peers() const { return default_max_peers_; }
        void set_default_listen_port(int32_t v) { default_listen_port_ = v; next_listen_port_ = v; }
        int32_t default_listen_port() const { return default_listen_port_; }
        void set_default_listen_port_end(int32_t v) { default_listen_port_end_ = v; }
        int32_t default_listen_port_end() const { return default_listen_port_end_; }
        void set_default_download_limit_kbps(int32_t v) { default_download_limit_kbps_ = v; }
        int32_t default_download_limit_kbps() const { return default_download_limit_kbps_; }
        void set_default_upload_limit_kbps(int32_t v) { default_upload_limit_kbps_ = v; }
        int32_t default_upload_limit_kbps() const { return default_upload_limit_kbps_; }
        void set_default_nat_config(const yuan::net::bit_torrent::NatConfig &cfg) { default_nat_config_ = cfg; }
        const yuan::net::bit_torrent::NatConfig &default_nat_config() const { return default_nat_config_; }
        std::unordered_map<int64_t, std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient>> active_clients() const;
        yuan::net::bit_torrent::UpnpManager *nat_manager() { return nat_manager_.get(); }
        bool shared_dht_running() const;
        size_t shared_dht_routing_table_size() const;

    private:
        ManagedTask *find_task_nolock(int64_t task_id);
        const ManagedTask *find_task_nolock(int64_t task_id) const;
        bool load_task_into_client_nolock(ManagedTask &task, yuan::net::bit_torrent::BitTorrentClient &client, std::string *error);
        void load_tasks_from_disk_nolock();
        void persist_tasks_to_disk_nolock() const;
        static uint64_t now_ms();
        void setup_client_callbacks(yuan::net::bit_torrent::BitTorrentClient &client, int64_t task_id);
        void try_start_queued_tasks_nolock();
        void apply_default_settings(yuan::net::bit_torrent::BitTorrentClient &client);
        void add_nat_port_mapping_for_client(yuan::net::bit_torrent::BitTorrentClient &client);
        void remove_nat_port_mapping_for_client(yuan::net::bit_torrent::BitTorrentClient &client);
        void setup_shared_dht_for_client(yuan::net::bit_torrent::BitTorrentClient &client);
        uint16_t allocate_listen_port();
        void release_listen_port(uint16_t port);

        std::string torrent_file_path_;
        std::string save_path_;
        ServerRuntimeHost host_;
        yuan::net::NetworkRuntime *shared_runtime_ = nullptr;

        std::unique_ptr<yuan::net::NetworkRuntime> bt_runtime_;
        std::thread bt_runtime_thread_;
        std::unique_ptr<yuan::net::bit_torrent::UpnpManager> nat_manager_;
        std::unique_ptr<yuan::net::bit_torrent::DhtNode> shared_dht_node_;

        int32_t default_max_peers_ = 50;
        int32_t default_listen_port_ = 6881;
        int32_t default_listen_port_end_ = 6999;
        int32_t next_listen_port_ = 6881;
        int32_t default_download_limit_kbps_ = 0;
        int32_t default_upload_limit_kbps_ = 0;
        yuan::net::bit_torrent::NatConfig default_nat_config_;

        mutable std::mutex tasks_mutex_;
        std::vector<ManagedTask> tasks_;
        int64_t next_task_id_ = 1;
        std::unordered_map<int64_t, std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient>> active_clients_;
        std::unordered_map<int64_t, std::shared_ptr<yuan::net::bit_torrent::BitTorrentClient>> seeding_clients_;
        int32_t max_concurrent_downloads_ = 3;
        std::unordered_set<uint16_t> used_ports_;
    };

} // namespace yuan::server

#endif
