#include "runtime/download_runtime_coordinator.h"

#include "nat/nat_manager.h"
#include "net/runtime/network_runtime.h"

namespace yuan::net::bit_torrent
{
    namespace
    {
        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }


    void DownloadRuntimeCoordinator::configure(DownloadRuntimeConfig config)
    {
        config_ = std::move(config);
    }

    bool DownloadRuntimeCoordinator::start(bool defer_bootstrap_to_loop)
    {
        if (!config_.runtime_ || !config_.meta_ ||
            !config_.peer_id_ || !config_.pieces_have_ || !config_.listen_port_ ||
            !config_.stats_tracker_) {
            return false;
        }

        ensure_sessions();
        configure_peer_session();
        configure_tracker_session();
        tracker_session_->start();

        if (defer_bootstrap_to_loop) {
            config_.runtime_->dispatch([this]() {
            bootstrap_runtime();
            });
        } else {
            bootstrap_runtime();
        }

        schedule_stats_updates();
        return true;
    }

    void DownloadRuntimeCoordinator::stop()
    {
        if (stats_timer_) {
            stats_timer_.cancel();
            stats_timer_.reset();
        }

        if (tracker_session_) {
            tracker_session_->stop();
        }

        if (peer_session_) {
            peer_session_->set_nat_manager(nullptr);
            peer_session_->disconnect_all_peers();
        }

        if (nat_manager_) {
            nat_manager_->stop();
            nat_manager_.reset();
        }
    }

    void DownloadRuntimeCoordinator::announce_now()
    {
        if (tracker_session_) {
            tracker_session_->announce_now();
        }
    }

    void DownloadRuntimeCoordinator::broadcast_have(uint32_t piece_index)
    {
        if (peer_session_) {
            peer_session_->broadcast_have(piece_index);
        }
    }

    std::vector<std::shared_ptr<PeerConnection> > DownloadRuntimeCoordinator::get_active_peers() const
    {
        return peer_session_ ? peer_session_->get_active_peers() : std::vector<std::shared_ptr<PeerConnection> >{};
    }

    int32_t DownloadRuntimeCoordinator::get_peer_count() const
    {
        return peer_session_ ? peer_session_->get_peer_count() : 0;
    }

    int32_t DownloadRuntimeCoordinator::get_active_peer_count() const
    {
        return peer_session_ ? peer_session_->get_active_peer_count() : 0;
    }

    void DownloadRuntimeCoordinator::ensure_sessions()
    {
        if (!tracker_session_) {
            tracker_session_ = std::make_shared<TrackerSession>();
        }

        if (!peer_session_) {
            peer_session_ = std::make_unique<PeerSession>();
        }
    }

    void DownloadRuntimeCoordinator::configure_peer_session()
    {
        PeerSessionConfig peer_config;
        peer_config.runtime_ = config_.runtime_;
        peer_config.nat_manager_ = ptr_of(nat_manager_);
        peer_config.meta_ = config_.meta_;
        peer_config.peer_id_ = config_.peer_id_;
        peer_config.pieces_have_ = config_.pieces_have_;
        peer_config.max_peers_ = config_.nat_config_.max_active_connections;
        peer_config.allow_loopback_peers_ = config_.nat_config_.allow_loopback_peers;
        peer_config.piece_data_handler_ = config_.piece_data_handler_;
        peer_config.piece_request_handler_ = config_.piece_request_handler_;
        peer_config.piece_served_handler_ = config_.piece_served_handler_;
        peer_config.peer_ready_handler_ = config_.peer_ready_handler_;
        peer_config.peer_unchoke_handler_ = config_.peer_unchoke_handler_;
        peer_config.peer_reject_handler_ = config_.peer_reject_handler_;
        peer_config.peer_lost_handler_ = config_.peer_lost_handler_;
        peer_session_->configure(std::move(peer_config));
    }

    void DownloadRuntimeCoordinator::configure_tracker_session()
    {
        TrackerSessionConfig tracker_config;
        tracker_config.runtime_ = config_.runtime_;
        tracker_config.peer_list_handler_ = [this](const std::vector<PeerAddress> &peers) {
        if (peer_session_)
        {
            peer_session_->connect_peers(peers);
        }
        };
        tracker_config.context_provider_ = [this]()->TrackerAnnounceContext
        {
            return config_.stats_tracker_->make_tracker_context(*config_.meta_, *config_.listen_port_);
        };
        tracker_session_->configure(std::move(tracker_config));
    }

    void DownloadRuntimeCoordinator::start_nat_runtime()
    {
        const auto &cfg = config_.nat_config_;
        if (!cfg.enable_upnp && !cfg.enable_nat_pmp && !cfg.enable_dht) {
            return;
        }

        nat_manager_ = std::make_unique<NatManager>();
        config_.nat_config_.listen_port = *config_.listen_port_;
        nat_manager_->start(config_.nat_config_, *config_.meta_, *config_.peer_id_,
                            *config_.pieces_have_, config_.runtime_);

        peer_session_->set_nat_manager(ptr_of(nat_manager_));
        configure_peer_session();
        peer_session_->bind_nat_runtime();
    }

    void DownloadRuntimeCoordinator::update_listen_port_from_nat()
    {
        if (!nat_manager_) {
            return;
        }

        if (nat_manager_->is_upnp_mapped() || nat_manager_->is_listening()) {
            *config_.listen_port_ = nat_manager_->get_external_port();
        }
    }

    void DownloadRuntimeCoordinator::bootstrap_runtime()
    {
        start_nat_runtime();
        update_listen_port_from_nat();
        tracker_session_->announce_now();
    }

    void DownloadRuntimeCoordinator::schedule_stats_updates()
    {
        stats_timer_ = config_.runtime_->schedule_periodic(
            5000,
            5000,
            [this]() {
            config_.stats_tracker_->update_peer_counts(get_active_peer_count(), get_peer_count());
            config_.stats_tracker_->emit();
            },
            -1);
    }

} // namespace yuan::net::bit_torrent
