#include "session/peer_session.h"
#include "net/connector/tcp_connector.h"
#include "net/socket/inet_address.h"
#include "net/runtime/network_runtime.h"

namespace yuan::net::bit_torrent
{

    void PeerSession::configure(PeerSessionConfig config)
    {
        runtime_ = config.runtime_;
        nat_manager_ = config.nat_manager_;
        meta_ = config.meta_;
        peer_id_ = config.peer_id_;
        pieces_have_ = config.pieces_have_;
        max_peers_ = config.max_peers_;
        allow_loopback_peers_ = config.allow_loopback_peers_;
        piece_data_handler_ = std::move(config.piece_data_handler_);
        piece_request_handler_ = std::move(config.piece_request_handler_);
        piece_served_handler_ = std::move(config.piece_served_handler_);
        peer_ready_handler_ = std::move(config.peer_ready_handler_);
        peer_lost_handler_ = std::move(config.peer_lost_handler_);
    }

    void PeerSession::bind_nat_runtime()
    {
        if (!nat_manager_) {
            return;
        }

        nat_manager_->set_peer_callback([this](std::shared_ptr<PeerConnection> peer) {
        on_inbound_peer(std::move(peer));
        });

        nat_manager_->set_dht_peer_callback([this](const std::vector<PeerAddress> &peers) {
        connect_peers(peers);
        });
    }

    void PeerSession::set_runtime(net::NetworkRuntime * runtime)
    {
        runtime_ = runtime;
    }

    void PeerSession::set_nat_manager(NatManager * nat_manager)
    {
        nat_manager_ = nat_manager;
    }

    void PeerSession::set_context(const TorrentMeta * meta,
                                  const std::string * peer_id,
                                  const std::vector<bool> * pieces_have)
    {
        meta_ = meta;
        peer_id_ = peer_id;
        pieces_have_ = pieces_have;
    }

    std::string PeerSession::make_key(const std::string & ip, uint16_t port) const
    {
        return ip + ":" + std::to_string(port);
    }

    void PeerSession::attach_peer(const std::shared_ptr<PeerConnection> &peer, const std::string & key)
    {
        if (!peer) {
            return;
        }

        (void)key;
        peer->set_piece_data_handler(piece_data_handler_);
        peer->set_piece_request_handler(piece_request_handler_);
        peer->set_piece_served_handler(piece_served_handler_);
        peer->set_on_state_change([this](PeerConnection *p) {
        on_peer_state_changed(p);
        });
    }

    void PeerSession::on_peer_state_changed(PeerConnection * peer)
    {
        if (!peer) {
            return;
        }

        const auto state = peer->get_state();
        if (state == PeerConnection::State::connected) {
            on_peer_connected(*peer);
            return;
        }

        if (state == PeerConnection::State::closed || state == PeerConnection::State::error) {
            if (peer_lost_handler_) {
                const auto pending_requests = peer->take_pending_requests();
                if (!pending_requests.empty()) {
                    peer_lost_handler_(pending_requests);
                }
            }
            remove_peer(*peer);
        }
    }

    void PeerSession::on_peer_connected(PeerConnection & peer)
    {
        if (nat_manager_) {
            nat_manager_->register_peer(&peer, make_key(peer.get_peer_ip(), peer.get_peer_port()));
        }

        if (peer_ready_handler_) {
            peer_ready_handler_(&peer);
        }
    }

    void PeerSession::remove_peer(PeerConnection & peer)
    {
        std::string key;
        {
        std::lock_guard<std::mutex> lock(peers_mutex_);
            for (auto it = peers_.begin(); it != peers_.end(); ++it) {
                if (it->second && &*it->second == &peer) {
                    key = it->first;
                    peers_.erase(it);
                    break;
                }
            }
        }

        if (!key.empty() && nat_manager_) {
            nat_manager_->unregister_peer(key);
        }
    }

    void PeerSession::connect_peers(const std::vector<PeerAddress> & peer_list)
    {
        if (!meta_ || !peer_id_ || !pieces_have_ || !runtime_) {
            return;
        }

        for (const auto &addr : peer_list) {
            std::shared_ptr<PeerConnection> peer;
            std::string key;
            {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                if (static_cast<int32_t>(peers_.size()) >= max_peers_) {
                    break;
                }

                key = make_key(addr.ip_, addr.port_);
                if (peers_.count(key)) {
                    continue;
                }

                if (addr.ip_ == "0.0.0.0") {
                    continue;
                }

                if (!allow_loopback_peers_ && addr.ip_ == "127.0.0.1") {
                    continue;
                }

                peer = std::make_shared<PeerConnection>();
                peers_[key] = peer;
            }

            if (!peer) {
                continue;
            }

            attach_peer(peer, key);
            peer->connect(addr.ip_, addr.port_, *meta_, *peer_id_, runtime_);
        }
    }

    void PeerSession::on_inbound_peer(std::shared_ptr<PeerConnection> peer)
    {
        if (!peer || !meta_ || !peer_id_ || !pieces_have_) {
            return;
        }

        const std::string key = make_key(peer->get_peer_ip(), peer->get_peer_port());

        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (static_cast<int32_t>(peers_.size()) >= max_peers_ || peers_.count(key)) {
                return;
            }

            peers_[key] = peer;
        }

        attach_peer(peer, key);

        if (peer->is_connected()) {
            on_peer_connected(*peer);
        }
    }

    void PeerSession::disconnect_all_peers()
    {
        std::vector<std::shared_ptr<PeerConnection> > peers;
        std::vector<std::string> keys;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            for (auto &pair : peers_) {
                peers.push_back(pair.second);
                keys.push_back(pair.first);
            }
            peers_.clear();
        }

        if (nat_manager_) {
            for (const auto &key : keys) {
                nat_manager_->unregister_peer(key);
            }
        }

        for (const auto &peer : peers) {
            if (peer) {
                peer->disconnect();
            }
        }
    }

    void PeerSession::broadcast_have(uint32_t piece_index)
    {
        for (const auto &peer : get_active_peers()) {
            if (peer) {
                peer->send_have(piece_index);
            }
        }
    }

    std::vector<std::shared_ptr<PeerConnection> > PeerSession::get_active_peers() const
    {
        std::vector<std::shared_ptr<PeerConnection> > peers;
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto &pair : peers_) {
            if (pair.second && pair.second->is_connected()) {
                peers.push_back(pair.second);
            }
        }
        return peers;
    }

    int32_t PeerSession::get_peer_count() const
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        return static_cast<int32_t>(peers_.size());
    }

    int32_t PeerSession::get_active_peer_count() const
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        int32_t count = 0;
        for (const auto &pair : peers_) {
            if (pair.second && pair.second->is_connected()) {
                ++count;
            }
        }
        return count;
    }

} // namespace yuan::net::bit_torrent
