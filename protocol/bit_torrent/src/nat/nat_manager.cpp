#include "nat/nat_manager.h"
#include "nat/peer_listener.h"
#include "nat/upnp_manager.h"
#include "nat/utp_connection.h"
#include "nat/dht_node.h"
#include "nat/pex_manager.h"
#include "peer_wire/peer_wire_message.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include <cstring>

namespace yuan::net::bit_torrent
{

// Lambda timer task adapter
class LambdaTimerTask : public timer::TimerTask
{
public:
    explicit LambdaTimerTask(std::function<void(timer::Timer *)> fn) : fn_(std::move(fn)) {}
    void on_timer(timer::Timer *t) override { fn_(t); }
private:
    std::function<void(timer::Timer *)> fn_;
};

NatManager::NatManager() {}

NatManager::~NatManager()
{
    stop();
}

void NatManager::start(const NatConfig &config,
                        const TorrentMeta &meta,
                        const std::string &peer_id,
                        const std::vector<bool> &pieces_have,
                        net::EventLoop *loop,
                        timer::TimerManager *timer_mgr)
{
    if (started_) return;

    config_ = config;
    ev_loop_ = loop;
    timer_manager_ = timer_mgr;
    info_hash_ = meta.info_hash_;
    peer_id_ = peer_id;
    pieces_have_ = &pieces_have;
    started_ = true;

    // 1. Start inbound listener (TCP)
    if (config_.enable_inbound_listen)
    {
        peer_listener_ = std::make_unique<PeerListener>();
        peer_listener_->start(config, meta, peer_id, pieces_have, loop, timer_mgr);
        peer_listener_->set_new_peer_callback([this](PeerConnection *peer)
        {
            on_new_tcp_peer(peer);
        });
    }

    // 2. Start UPnP / NAT-PMP port mapping
    if (config_.enable_upnp || config_.enable_nat_pmp)
    {
        upnp_manager_ = std::make_unique<UpnpManager>();
        int32_t map_port = peer_listener_ ? peer_listener_->get_actual_port() : config_.listen_port;
        upnp_manager_->start(config, static_cast<uint16_t>(map_port),
                             [this](bool success, const std::string &ip, uint16_t port)
        {
            on_upnp_result(success, ip, port);
        });
    }

    // 3. Start uTP (UDP transport)
    if (config_.enable_utp)
    {
        utp_manager_ = std::make_unique<UtpManager>();
        if (utp_manager_->start(config, loop, timer_mgr))
        {
            utp_manager_->set_new_peer_callback([this](UtpConnection *utp_conn)
            {
                on_new_utp_peer(utp_conn);
            });
        }
        else
        {
            utp_manager_.reset();
        }
    }

    // 4. Start DHT
    if (config_.enable_dht)
    {
        dht_node_ = std::make_unique<DhtNode>();
        if (dht_node_->start(config, loop, timer_mgr, external_ip_))
        {
            dht_node_->set_node_callback([](const DhtCompactNode &node)
            {
                // New DHT node discovered - could add to PEX
            });

            // Announce ourselves to DHT
            int32_t announce_port = peer_listener_ ? peer_listener_->get_actual_port() : config_.listen_port;
            dht_node_->announce(info_hash_, static_cast<uint16_t>(announce_port));

            // Set DHT peer callback
            dht_node_->set_node_callback([this](const DhtCompactNode &) {});
        }
        else
        {
            dht_node_.reset();
        }
    }

    // 5. Start PEX
    if (config_.enable_pex)
    {
        pex_manager_ = std::make_unique<PexManager>();
        pex_manager_->init(info_hash_, config);
        pex_manager_->set_new_peer_callback([this](const std::vector<PexPeerInfo> &peers)
        {
            // New peers discovered via PEX
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
    if (!started_) return;
    started_ = false;

    if (peer_listener_) { peer_listener_->stop(); peer_listener_.reset(); }
    if (upnp_manager_) { upnp_manager_->stop(); upnp_manager_.reset(); }
    if (utp_manager_) { utp_manager_->stop(); utp_manager_.reset(); }
    if (dht_node_) { dht_node_->stop(); dht_node_.reset(); }
    if (pex_manager_) { pex_manager_.reset(); }

    ev_loop_ = nullptr;
    timer_manager_ = nullptr;
    pieces_have_ = nullptr;
}

void NatManager::register_peer(PeerConnection *peer, const std::string &key)
{
    // Notify PEX manager about this peer
    if (pex_manager_)
    {
        pex_manager_->add_peer(peer->get_peer_ip(), peer->get_peer_port());
    }

    // If peer supports extension protocol, send our extension handshake
    if (peer->is_connected())
    {
        // Send extension handshake via the extended message (id=20, ext_id=0)
        // This requires modifying PeerConnection to support extended messages.
        // For now, PEX is passive - we respond to incoming ext handshakes.
    }
}

void NatManager::unregister_peer(const std::string &key)
{
    if (pex_manager_)
    {
        pex_manager_->remove_peer(key);
    }
}

void NatManager::on_pieces_changed(const std::vector<bool> &pieces_have)
{
    pieces_have_ = &pieces_have;

    // Re-announce to DHT if we have new pieces
    if (dht_node_ && dht_node_->is_running())
    {
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

void NatManager::on_upnp_result(bool success, const std::string &ip, uint16_t port)
{
    if (success)
    {
        external_ip_ = ip;
        external_port_ = port;
        external_address_resolved_ = true;
    }
}

void NatManager::on_new_tcp_peer(PeerConnection *peer)
{
    if (peer_cb_)
        peer_cb_(peer);
}

void NatManager::on_new_utp_peer(UtpConnection *utp_conn)
{
    // For uTP peers, we need to:
    // 1. Complete the BT handshake over uTP
    // 2. Create a PeerConnection wrapper (or handle separately)
    // For now, uTP connections just complete the transport layer.
    // The BT handshake would be sent/received via the uTP data handler.

    // Set up data handler to receive BT protocol data
    utp_conn->set_data_handler([this, utp_conn](const uint8_t *data, size_t len)
    {
        // BT handshake + PWP messages would be processed here
        // For now, this is a placeholder for the full uTP integration
    });

    // Send BT handshake over uTP
    HandshakeMessage hs;
    hs.set_info_hash(info_hash_.data());
    hs.set_peer_id(peer_id_);
    auto hs_data = hs.serialize();
    utp_conn->send_data(hs_data.data(), hs_data.size());
}

void NatManager::on_dht_peers(const std::vector<PeerAddress> &peers)
{
    if (dht_peer_cb_)
        dht_peer_cb_(peers);
}

} // namespace yuan::net::bit_torrent
