#include "nat/nat_manager.h"
#include "nat/peer_listener.h"
#include "nat/upnp_manager.h"
#include "nat/utp_connection.h"
#include "nat/dht_node.h"
#include "nat/pex_manager.h"
#include "peer_wire/peer_wire_message.h"
#include "peer_wire/peer_connection.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include <cstring>

namespace yuan::net::bit_torrent
{

    NatManager::NatManager()
    {
    }

    NatManager::~NatManager()
    {
        stop();
    }

    void NatManager::start(const NatConfig & config,
                           const TorrentMeta & meta,
                           const std::string & peer_id,
                           const std::vector<bool> & pieces_have,
                           net::NetworkRuntime * runtime)
    {
        if (started_)
            return;

        config_ = config;
        runtime_ = runtime;
        info_hash_ = meta.info_hash_;
        peer_id_ = peer_id;
        pieces_have_ = &pieces_have;
        started_ = true;

        if (config_.enable_inbound_listen) {
            peer_listener_ = std::make_unique<PeerListener>();
            peer_listener_->start(config, meta, peer_id, pieces_have, runtime);
            peer_listener_->set_new_peer_callback([this](std::shared_ptr<PeerConnection> peer) {
            on_new_tcp_peer(std::move(peer));
            });
        }

        if (config_.enable_upnp || config_.enable_nat_pmp) {
            upnp_manager_ = std::make_unique<UpnpManager>();
            int32_t map_port = peer_listener_ ? peer_listener_->get_actual_port() : config_.listen_port;
            upnp_manager_->start(config, static_cast<uint16_t>(map_port),
                                 [this](bool success, const std::string &ip, uint16_t port) {
            on_upnp_result(success, ip, port);
            });
        }

        if (config_.enable_utp) {
            utp_manager_ = std::make_unique<UtpManager>();
            if (utp_manager_->start(config, runtime)) {
                utp_manager_->set_new_peer_callback([this](UtpConnection *utp_conn) {
                on_new_utp_peer(utp_conn);
                });
            } else {
                utp_manager_.reset();
            }
        }

        if (config_.enable_dht) {
            dht_node_ = std::make_unique<DhtNode>();
            if (dht_node_->start(config, runtime, external_ip_)) {
                int32_t announce_port = peer_listener_ ? peer_listener_->get_actual_port() : config_.listen_port;
                dht_node_->announce(info_hash_, static_cast<uint16_t>(announce_port));
            } else {
                dht_node_.reset();
            }
        }

        if (config_.enable_pex) {
            pex_manager_ = std::make_unique<PexManager>();
            pex_manager_->init(info_hash_, config);
            pex_manager_->set_new_peer_callback([this](const std::vector<PexPeerInfo> &peers) {
            if (dht_peer_cb_)
            {
                std::vector<PeerAddress> addrs;
                for (const auto &p : peers)
                    addrs.push_back({p.ip, p.port});
                dht_peer_cb_(addrs);
            }
            });
        }
    }

    void NatManager::stop()
    {
        if (!started_)
            return;
        started_ = false;

        if (peer_listener_) {
            peer_listener_->stop();
            peer_listener_.reset();
        }
        if (upnp_manager_) {
            upnp_manager_->stop();
            upnp_manager_.reset();
        }
        if (utp_manager_) {
            utp_manager_->stop();
            utp_manager_.reset();
        }
        if (dht_node_) {
            dht_node_->stop();
            dht_node_.reset();
        }
        if (pex_manager_) {
            pex_manager_.reset();
        }
        utp_peers_.clear();

        runtime_ = nullptr;
        pieces_have_ = nullptr;
    }

    void NatManager::register_peer(PeerConnection * peer, const std::string & key)
    {
        if (pex_manager_) {
            pex_manager_->add_peer(peer->get_peer_ip(), peer->get_peer_port());

            if (peer->get_peer_state().supports_extensions) {
                auto ext_hs = pex_manager_->build_ext_handshake();
                peer->send_extended(0, ext_hs.data(), ext_hs.size());

                peer->set_extended_message_handler(
                    [this, key](PeerConnection *, uint8_t ext_id,
                                const uint8_t *payload, size_t len) {
                        pex_manager_->on_extended_message(key, ext_id, payload, len);
                    });
            }
        }
    }

    void NatManager::unregister_peer(const std::string & key)
    {
        if (pex_manager_) {
            pex_manager_->remove_peer(key);
        }
    }

    void NatManager::on_pieces_changed(const std::vector<bool> & pieces_have)
    {
        pieces_have_ = &pieces_have;

        // Re-announce to DHT if we have new pieces
        if (dht_node_ && dht_node_->is_running()) {
            int32_t announce_port = peer_listener_ ? peer_listener_->get_actual_port() : config_.listen_port;
            dht_node_->announce(info_hash_, static_cast<uint16_t>(announce_port));
        }
    }

    std::string NatManager::get_external_ip() const
    {
        if (upnp_manager_ && upnp_manager_->is_mapped())
            return upnp_manager_->get_external_ip();
        if (!config_.external_ip.empty())
            return config_.external_ip;
        return external_ip_;
    }

    uint16_t NatManager::get_external_port() const
    {
        if (upnp_manager_ && upnp_manager_->is_mapped())
            return upnp_manager_->get_mapped_port();
        if (peer_listener_)
            return static_cast<uint16_t>(peer_listener_->get_actual_port());
        return static_cast<uint16_t>(config_.listen_port);
    }

    bool NatManager::is_listening() const
    {
        return peer_listener_ && peer_listener_->is_listening();
    }

    bool NatManager::is_upnp_mapped() const
    {
        return upnp_manager_ && upnp_manager_->is_mapped();
    }

    bool NatManager::is_dht_running() const
    {
        return dht_node_ && dht_node_->is_running();
    }

    bool NatManager::is_utp_running() const
    {
        return utp_manager_ && utp_manager_->is_running();
    }

    void NatManager::on_upnp_result(bool success, const std::string & ip, uint16_t port)
    {
        if (success) {
            external_ip_ = ip;
            external_port_ = port;
            external_address_resolved_ = true;
        }
    }

    void NatManager::on_new_tcp_peer(std::shared_ptr<PeerConnection> peer)
    {
        if (peer_cb_)
            peer_cb_(std::move(peer));
    }

    void NatManager::on_new_utp_peer(UtpConnection * utp_conn)
    {
        auto peer = std::make_shared<PeerConnection>();
        peer->setup_utp(info_hash_, peer_id_,
                        utp_conn->get_remote_ip(), utp_conn->get_remote_port(),
                        pieces_have_ ? static_cast<int32_t>(pieces_have_->size()) : 0,
                        runtime_);

        peer->set_send_handler([utp_conn](const uint8_t *data, size_t len) {
            utp_conn->send_data(data, len);
        });

        const std::string key = peer->get_peer_ip() + ":" + std::to_string(peer->get_peer_port());
        utp_peers_[key] = peer;

        peer->set_on_state_change([this, key, peer](PeerConnection *p) {
            if (p->get_state() == PeerConnection::State::connected) {
                if (peer_cb_)
                    peer_cb_(peer);

                register_peer(p, key);
            } else if (p->get_state() == PeerConnection::State::closed ||
                       p->get_state() == PeerConnection::State::error) {
                unregister_peer(key);
                utp_peers_.erase(key);
            }
        });

        utp_conn->set_data_handler([peer](const uint8_t *data, size_t len) {
            peer->feed_data(data, len);
        });

        HandshakeMessage hs;
        hs.set_info_hash(info_hash_.data());
        hs.set_peer_id(peer_id_);
        auto hs_data = hs.serialize();
        utp_conn->send_data(hs_data.data(), hs_data.size());
    }

    void NatManager::on_dht_peers(const std::vector<PeerAddress> & peers)
    {
        if (dht_peer_cb_)
            dht_peer_cb_(peers);
    }

} // namespace yuan::net::bit_torrent
