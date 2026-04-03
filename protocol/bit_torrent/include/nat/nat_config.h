#ifndef __BIT_TORRENT_NAT_CONFIG_H__
#define __BIT_TORRENT_NAT_CONFIG_H__

#include <string>
#include <cstdint>

namespace yuan::net::bit_torrent
{

// Centralized configuration for all NAT traversal features.
// Each feature can be independently enabled/disabled.
struct NatConfig
{
    // ===== Inbound Listening =====
    bool enable_inbound_listen = true;  // Accept incoming peer connections (default: ON)
    int32_t listen_port = 6881;         // Port to listen on
    int32_t listen_retry_range = 10;    // Try ports [listen_port, listen_port + range] if occupied

    // ===== UPnP / NAT-PMP Port Mapping =====
    bool enable_upnp = true;            // UPnP IGD port mapping (default: ON, most mature)
    bool enable_nat_pmp = true;         // NAT-PMP / PCP port mapping (default: ON)
    uint32_t upnp_discover_timeout_ms = 3000;  // SSDP discovery timeout
    uint32_t upnp_lease_duration = 3600;       // Lease duration in seconds

    // ===== uTP (BEP 29) =====
    bool enable_utp = true;             // Micro Transport Protocol over UDP (default: ON)
    int32_t utp_port = 6882;            // UDP port for uTP (0 = same as listen_port)
    uint32_t utp_connect_timeout_ms = 15000;
    uint32_t utp_syn_retry = 3;

    // ===== DHT (BEP 5) =====
    bool enable_dht = true;             // Kademlia DHT (default: ON)
    int32_t dht_port = 6882;            // DHT UDP port
    uint32_t dht_max_nodes = 200;       // Max nodes in routing table
    uint32_t dht_refresh_interval_s = 900; // Bucket refresh interval

    // ===== PEX (BEP 10) =====
    bool enable_pex = true;             // Peer Exchange extension (default: ON)
    uint32_t pex_max_peers = 100;       // Max peers to remember for PEX
    uint32_t pex_flush_interval_s = 60; // How often to send PEX messages

    // ===== General =====
    std::string external_ip;            // Manually set external IP (empty = auto-detect)
    int32_t max_active_connections = 100; // Total max connections (inbound + outbound)
};

} // namespace yuan::net::bit_torrent

#endif // __BIT_TORRENT_NAT_CONFIG_H__
