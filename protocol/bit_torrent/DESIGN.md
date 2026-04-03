# BitTorrent P2P Protocol Design Document

## 1. Overview

This document describes the design of the BitTorrent P2P protocol implementation
under `protocol/bit_torrent/`, covering tracker communication, the Peer Wire Protocol,
and a complete NAT traversal subsystem. The design references mainstream
implementations such as **aria2**, **libtorrent**, and the official BEP specifications.

### Supported BEPs

| BEP | Name | Status |
|-----|------|--------|
| BEP 3 | BitTorrent Protocol | Implemented |
| BEP 5 | DHT (Kademlia) | Implemented |
| BEP 9 | Extension for Peers to Send Metadata Files | Planned |
| BEP 10 | Extension Protocol (PEX) | Implemented |
| BEP 12 | Multitracker Metadata Extension | Implemented |
| BEP 15 | UDP Tracker Protocol | Implemented |
| BEP 27 | Private Torrents | Implemented |
| BEP 29 | uTP (Micro Transport Protocol) | Implemented |
| UPnP IGD | Universal Plug and Play | Implemented |
| NAT-PMP/PCP | Port Mapping Protocol | Implemented |

## 2. Architecture

### 2.1 Module Structure

```
protocol/bit_torrent/
├── include/
│   ├── bit_torrent_client.h           # Unified client API (aria2-style)
│   ├── torrent_meta.h                  # .torrent file metadata parser
│   ├── utils.h                         # SHA-1, URL encoding, peer ID generation
│   ├── structure/
│   │   └── bencoding.h                 # BEncode codec (BEP 3)
│   ├── tracker/
│   │   ├── http_tracker.h              # HTTP/HTTPS tracker (BEP 3)
│   │   └── udp_tracker.h               # UDP tracker (BEP 15)
│   ├── peer_wire/
│   │   ├── peer_wire_message.h         # PWP message definitions (BEP 3)
│   │   └── peer_connection.h           # TCP peer connection (BEP 3)
│   └── nat/
│       ├── nat_config.h                # Centralized NAT configuration
│       ├── nat_manager.h               # Unified NAT traversal orchestrator
│       ├── peer_listener.h             # TCP inbound peer listener
│       ├── upnp_manager.h              # UPnP IGD / NAT-PMP port mapping
│       ├── utp_connection.h            # uTP protocol (BEP 29)
│       ├── dht_node.h                  # Kademlia DHT (BEP 5)
│       └── pex_manager.h              # Peer Exchange (BEP 10)
└── src/
    ├── bit_torrent_client.cpp
    ├── torrent_meta.cpp
    ├── utils.cpp
    ├── structure/bencoding.cpp
    ├── tracker/http_tracker.cpp
    ├── tracker/udp_tracker.cpp
    ├── peer_wire/peer_connection.cpp
    └── nat/
        ├── nat_manager.cpp
        ├── peer_listener.cpp
        ├── upnp_manager.cpp
        ├── utp_connection.cpp
        ├── dht_node.cpp
        └── pex_manager.cpp
```

### 2.2 Dependency Graph

```
BitTorrentClient
  ├── NatManager                    (NAT traversal orchestrator)
  │     ├── PeerListener           (TCP inbound, ConnectionHandler)
  │     │     └── TcpAcceptor, Socket, EventLoop
  │     ├── UpnpManager            (UPnP/NAT-PMP, background thread)
  │     │     └── SSDP multicast, SOAP, NAT-PMP UDP
  │     ├── UtpManager             (uTP over UDP, BEP 29)
  │     │     ├── UtpConnection    (single uTP connection, LEDBAT CC)
  │     │     └── UdpAcceptor, TimerTask
  │     ├── DhtNode                (Kademlia DHT, BEP 5)
  │     │     ├── Routing table (160 K-buckets)
  │     │     └── UdpAcceptor, TimerTask
  │     └── PexManager             (Peer Exchange, BEP 10)
  │           └── Extension handshake, ut_pex messages
  ├── HttpTracker                   (HTTP/HTTPS announce, BEP 3)
  ├── UdpTracker                    (UDP announce, BEP 15)
  │     └── EventLoop, UdpAcceptor, Buffer, TimerTask
  ├── PeerConnection                (P2P TCP, Peer Wire Protocol)
  │     └── TcpConnector, Connection, TimerTask
  ├── TorrentMeta                   (.torrent parser)
  │     └── Bencoding              (BEncode codec)
  └── utils                         (SHA-1, URL encode, peer_id)
        └── OpenSSL SHA-1

Core (net, event, timer, buffer, socket)
```

### 2.3 Thread Model

- **Main Thread**: API calls (`load_torrent`, `start`, `stop`), callbacks
- **I/O Thread**: Single `EventLoop` drives all network I/O (non-blocking)
  - Tracker HTTP requests: executed synchronously via raw socket
  - Tracker UDP: uses `UdpAcceptor` with `EventLoop`
  - Peer TCP connections (inbound/outbound): uses `TcpAcceptor` / `TcpConnector`
  - uTP: uses `UdpAcceptor` (shared or separate port)
  - DHT: uses `UdpAcceptor` (shared or separate port)
- **UPnP Worker Thread**: Background thread for SSDP discovery and SOAP requests
  - Communicates results back to the I/O thread via callbacks

```
┌─────────────┐     ┌──────────────────────────────────────────────┐
│ Main Thread │────>│ EventLoop (I/O Thread)                       │
│             │     │  ├── TimerManager                             │
│ load/start  │     │  ├── SelectPoller                             │
│ stop/stats  │     │  ├── TcpAcceptor  (inbound peers)             │
│             │     │  ├── TcpConnector (outbound peers)             │
│             │     │  ├── UdpAcceptor  (uTP + DHT)                  │
│             │     │  └── UdpAcceptor  (tracker, if UDP)           │
└─────────────┘     └──────────────────────────────────────────────┘
                            ^
┌─────────────┐           │ callback
│ UPnP Worker │───────────┘
│ (background)│  SSDP → SOAP → DeletePortMapping
└─────────────┘
```

## 3. Protocol Details

### 3.1 Torrent Metadata (`TorrentMeta`)

Parses `.torrent` files (BEncode format) to extract:

| Field | Description | BEP Reference |
|-------|-------------|---------------|
| `announce` | Primary tracker URL | BEP 3 |
| `announce-list` | Tiered tracker list | BEP 12 |
| `info.name` | Suggested file name | BEP 3 |
| `info.piece length` | Piece size in bytes | BEP 3 |
| `info.pieces` | Concatenated SHA-1 hashes (20 bytes each) | BEP 3 |
| `info.length` / `info.files` | Single-file or multi-file mode | BEP 3 |
| `info.private` | Private torrent flag | BEP 27 |

**Info Hash**: SHA-1 of the bencoded `info` dictionary (20 bytes).

### 3.2 Tracker Protocol - HTTP/HTTPS (`HttpTracker`)

**Standard**: BEP 3
**Request**: HTTP GET with query parameters

```
GET /announce?info_hash=%XX...&peer_id=-YZ0001-xxxxxxxxxxxx&port=6881
    &uploaded=0&downloaded=0&left=<remaining>&compact=1&event=started HTTP/1.1
```

**Response**: BEncoded dictionary with compact peer list (6 bytes per peer: 4 IP + 2 port).

### 3.3 Tracker Protocol - UDP (`UdpTracker`)

**Standard**: BEP 15

```
Client                              Tracker
  |── Connect (action=0) ──────────>|
  |<── Connect Response ────────────|  (conn_id, transaction_id)
  |── Announce (action=1) ─────────>|
  |<── Announce Response ───────────|  (interval, peers)
```

Uses `UdpAcceptor` + `EventLoop` for async I/O with timeout handling.

### 3.4 Peer Wire Protocol - TCP (`PeerConnection` + `PeerMessage`)

**Standard**: BEP 3

**Handshake** (68 bytes):
```
<1: protocol_len><19: "BitTorrent protocol"><8: reserved><20: info_hash><20: peer_id>
```

Reserved bytes indicate extensions:
- Bit 5 (byte 5): Fast extension (BEP 6)
- Bit 7 (byte 7): DHT support (BEP 5)

**Message Format**: `<4: length_prefix><1: message_id><payload>`

| ID | Name | Payload | Description |
|----|------|---------|-------------|
| 0 | choke | none | |
| 1 | unchoke | none | |
| 2 | interested | none | |
| 3 | not_interested | none | |
| 4 | have | 4 bytes (piece index) | |
| 5 | bitfield | variable | Bitfield of available pieces |
| 6 | request | 12 bytes (index, offset, length) | |
| 7 | piece | 8+length bytes | Block data |
| 8 | cancel | 12 bytes | Same as request |
| 9 | port | 2 bytes (DHT port) | BEP 5 |
| 20 | extended | variable | BEP 10 |

**Peer State Machine**:
```
idle -> connecting -> handshaking -> connected -> closed
                      |              |
                      +-------(error)-+
```

**Inbound Connections**:
`PeerConnection` supports inbound connections via `accept_inbound()`. The
`PeerListener` accepts TCP connections, validates the BT handshake (info_hash
match), sends our handshake reply, then hands off to `accept_inbound()` for
PWP message processing.

### 3.5 Piece Verification

Each downloaded piece is verified against its SHA-1 hash:
```
piece_hash = SHA1(piece_data)
expected_hash = meta.info.piece_hash(piece_index)
```

## 4. NAT Traversal Subsystem

### 4.1 Overview

The NAT traversal subsystem (`nat/`) enables peers behind NAT to both receive
inbound connections and discover peers without relying solely on trackers.
It is orchestrated by `NatManager` and consists of five independently
configurable components.

```
                    NatManager
                   /     |     \
            PeerListener  |   UpnpManager
            (TCP inbound) |   (port mapping)
                         UtpManager
                         (uTP/UDP transport)
                         /        \
                    DhtNode    PexManager
                    (BEP 5)    (BEP 10)
```

### 4.2 Configuration (`NatConfig`)

All features are independently toggleable via `NatConfig`:

```cpp
struct NatConfig
{
    // Inbound listening
    bool enable_inbound_listen = true;
    int32_t listen_port = 6881;
    int32_t listen_retry_range = 10;

    // UPnP / NAT-PMP
    bool enable_upnp = true;
    bool enable_nat_pmp = true;
    uint32_t upnp_discover_timeout_ms = 3000;
    uint32_t upnp_lease_duration = 3600;

    // uTP (BEP 29)
    bool enable_utp = true;
    int32_t utp_port = 6882;
    uint32_t utp_connect_timeout_ms = 15000;
    uint32_t utp_syn_retry = 3;

    // DHT (BEP 5)
    bool enable_dht = true;
    int32_t dht_port = 6882;
    uint32_t dht_max_nodes = 200;
    uint32_t dht_refresh_interval_s = 900;

    // PEX (BEP 10)
    bool enable_pex = true;
    uint32_t pex_max_peers = 100;
    uint32_t pex_flush_interval_s = 60;

    // General
    std::string external_ip;             // empty = auto-detect
    int32_t max_active_connections = 100;
};
```

### 4.3 Inbound Peer Listener (`PeerListener`)

Accepts inbound TCP connections on a configurable port range.

**Workflow**:
1. Create `TcpAcceptor`, bind to port (retry `listen_port` to `listen_port + range`)
2. On `on_connected`: create `PendingInbound`, start collecting 68-byte handshake
3. On `on_read`: accumulate handshake bytes, verify protocol string and info_hash
4. On valid handshake: send our handshake reply, call `peer->accept_inbound()`,
   invoke `new_peer_cb_`
5. On mismatch or error: close connection, discard peer

```
Remote Peer                    PeerListener                   BitTorrentClient
    |── SYN ────────────────>|                                |
    |<── SYN/ACK ────────────|                                |
    |── ACK ────────────────>|                                |
    |── BT Handshake (68B) ─>|  validate info_hash            |
    |                        |── reply handshake ────────────>|  accept_inbound()
    |<── BT Handshake ───────|                                |
    |── PWP messages ────────|───────────────────────────────>|
```

### 4.4 UPnP / NAT-PMP (`UpnpManager`)

Maps the local listening port on the router's external interface so peers
from the public internet can connect to us.

**UPnP IGD Workflow**:
1. SSDP M-SEARCH multicast to `239.255.255.250:1900` ( discovery )
2. Parse SSDP response, extract IGD location URL
3. Fetch IGD XML description, find WANIP/WANPPP service
4. SOAP `AddPortMapping` request: map external port -> internal port
5. Background thread renews lease before expiration
6. On stop: SOAP `DeletePortMapping` cleanup

**NAT-PMP / PCP Workflow**:
1. Detect gateway IP via `GetAdaptersInfo` (Windows) or `/proc/net/route` (Linux)
2. Send NAT-PMP external address request to gateway:5351
3. Send NAT-PMP port mapping request (UDP, map external port)
4. PCP fallback if NAT-PMP version indicates support

```
                  UPnP Manager (background thread)
                    /                \
          UPnP IGD              NAT-PMP/PCP
          SSDP multicast        Gateway UDP
          SOAP requests         Port mapping
              \                    /
               External IP + Port
                     |
              NatManager callback
```

### 4.5 uTP (`UtpConnection` + `UtpManager`)

Micro Transport Protocol (BEP 29) - a reliable transport over UDP with
LEDBAT congestion control, designed to minimize interference with other
traffic while enabling NAT hole punching.

**Packet Types**:

| Type | Name | Size | Purpose |
|------|------|------|---------|
| 0 | ST_RESET | 8 bytes | Abort connection |
| 1 | ST_STATE | 20 bytes | ACK only, no payload |
| 2 | ST_SYN | 20 bytes | Connection initiation |
| 3 | ST_DATA | 20+N bytes | Data with ACK |
| 4 | ST_FIN | 20 bytes | Graceful close |

**Header Format** (20 bytes):
```
<1: type_ver><1: version><1: extension><2: conn_id>
<4: seq_nr><4: ack_nr><4: timestamp><4: timestamp_diff>
[<1: window_size>]  (STATE/DATA/FIN only)
```

**Connection Lifecycle**:
```
Initiator:                     Responder:
  ST_SYN (conn_id+1)  -------->
                             <--------  ST_STATE (conn_id)
  ST_DATA (seq=1)     -------->
                             <--------  ST_STATE (ack=1)
  ...                                ...
  ST_FIN              -------->
                             <--------  ST_FIN
```

**LEDBAT Congestion Control**:
- Target delay: 100ms (less aggressive than TCP)
- `cwnd` grows/shrinks based on one-way delay measurements
- RTT estimated from timestamp differences
- Retransmission on timeout with exponential backoff

**NAT Hole Punching**:
When both peers know each other's external addresses, they can
simultaneously send ST_SYN to each other's external IP:port, creating
UDP state in both NATs and allowing direct communication.

**UtpManager** multiplexes multiple connections over a single UDP socket,
dispatching incoming packets by `conn_id`.

### 4.6 DHT (`DhtNode`)

Kademlia-based Distributed Hash Table (BEP 5) for trackerless peer discovery.

**Node ID**: 160-bit (20-byte), random on first start, persists across sessions.

**Routing Table**: 160 K-buckets (one per bit position), each holding up to K=8 nodes.
Buckets are organized by XOR distance from the local node ID.

**RPC Messages** (bencoded):

| Query | Description |
|-------|-------------|
| `ping` | Liveness check |
| `find_node` | Find K closest nodes to a target ID |
| `get_peers` | Get peers for an info_hash (returns nodes or compact peers) |
| `announce_peer` | Announce ourselves as a peer for an info_hash |

**Message Format**:
```
Query:    {"t":"<tid>", "y":"q", "q":"<method>", "a":{...}}
Response: {"t":"<tid>", "y":"r", "r":{...}}
Error:    {"t":"<tid>", "y":"e", "e":[<code>, "<msg>"]}
```

**Bootstrap Nodes**:
```
router.bittorrent.com:6881
dht.transmissionbt.com:6881
dht.aelitis.com:6881        (Vuze)
router.utorrent.com:6881
betadoctor.yellowmoz.com:6881
```

**Lookup Flow**:
```
get_peers(info_hash):
  1. Find alpha closest nodes from routing table
  2. Send get_peers to alpha nodes in parallel
  3. On response with "nodes": add to routing table, query closer ones
  4. On response with "values": return peer addresses
  5. Iteratively expand search until K closest nodes queried
```

**Periodic Maintenance**:
- Bucket refresh every 15 minutes
- Ping least-recently-seen nodes in buckets
- Token rotation for announce_peer (30-minute expiration)

### 4.7 PEX (`PexManager`)

Peer Exchange (BEP 10) allows connected peers to share their known peer lists,
reducing reliance on trackers and DHT for peer discovery.

**Extension Handshake** (message ID 20, ext_id 0):
```json
{"m": {"ut_pex": 5}, "v": "YZ 1.0", "yourip": "A", ...}
```

**ut_pex Message** (ext_id from handshake):
```json
{"added": "<compact_peer_list>", "added.f": "<flags>",
 "dropped": "<compact_peer_list>"}
```

- `added`: 6 bytes per peer (4-byte IP + 2-byte port, network order), plus
  optional flags byte per peer
- `dropped`: compact list of peers no longer connected
- PEX flushes accumulated peer changes every 60 seconds

**Compact Peer Format**: 6 bytes per peer = `{4-byte IP}{2-byte port}` (network order)

## 5. Key Design Decisions (vs aria2)

| Aspect | aria2 | This Implementation |
|--------|-------|-------------------|
| Event Loop | Multi-threaded `EpollEventPoll` + `CommandQueue` | Single `SelectPoller` (extensible) |
| Piece Selection | Rarest-first + sequential fallback | First-missing (simplifiable) |
| Choking Algorithm | Round-robin (BEP 3 spec) | N/A (client-only, no upload) |
| Tracker Fallback | Tier-based with retry | Tier-based, one per tier |
| Disk I/O | Memory-mapped + async write | Simple `fwrite` per piece |
| Extension Protocol | Full BEP 10 support | Implemented (ut_pex) |
| DHT | Full Kademlia DHT (BEP 5) | Implemented (K-buckets, bootstrapping) |
| uTP | Micro Transport Protocol (BEP 29) | Implemented (LEDBAT CC, hole punching) |
| UPnP | UPnP IGD port mapping | Implemented (SSDP + SOAP + NAT-PMP) |
| Inbound Listening | Yes | Implemented (TcpAcceptor) |
| Magnet Links | BEP 9 + BEP 5 DHT | Planned (needs ut_metadata) |

## 6. API Usage

### 6.1 Basic Download (with NAT traversal enabled by default)

```cpp
#include "bit_torrent_client.h"

int main() {
    yuan::net::bit_torrent::BitTorrentClient client;

    // 1. Load torrent
    client.load_torrent("example.torrent");
    client.set_save_path("./downloads");
    client.set_listen_port(6881);

    // 2. NAT traversal is enabled by default (inbound, UPnP, uTP, DHT, PEX)
    //    Customize if needed:
    yuan::net::bit_torrent::NatConfig nat_cfg;
    nat_cfg.listen_port = 6881;
    nat_cfg.enable_upnp = true;
    nat_cfg.enable_dht = true;
    nat_cfg.enable_pex = true;
    nat_cfg.enable_utp = true;
    client.set_nat_config(nat_cfg);

    // 3. Stats callback (called every 5 seconds)
    client.set_stats_callback([](const auto& stats) {
        printf("Progress: %.1f%% | Downloaded: %.2f MB | Upload: %.2f MB | "
               "Peers: %d/%d | Speed: %.1f KB/s\n",
               stats.progress_ * 100,
               stats.downloaded_bytes_ / 1048576.0,
               stats.uploaded_bytes_ / 1048576.0,
               stats.active_peers_, stats.total_peers_,
               stats.download_speed_ / 1024.0);
    });

    // 4. Start (blocking - runs EventLoop)
    client.start();

    return 0;
}
```

### 6.2 Minimal Download (no NAT, outbound only)

```cpp
yuan::net::bit_torrent::BitTorrentClient client;
client.load_torrent("example.torrent");

yuan::net::bit_torrent::NatConfig nat_cfg;
nat_cfg.enable_inbound_listen = false;
nat_cfg.enable_upnp = false;
nat_cfg.enable_dht = false;
nat_cfg.enable_pex = false;
nat_cfg.enable_utp = false;
client.set_nat_config(nat_cfg);

client.start();
```

### 6.3 Non-blocking Mode (external EventLoop)

```cpp
yuan::net::SelectPoller poller;
yuan::net::WheelTimerManager timer_mgr;
yuan::net::EventLoop loop(&poller, &timer_mgr);

yuan::net::bit_torrent::BitTorrentClient client;
client.set_event_loop(&loop);
client.set_timer_manager(&timer_mgr);
client.load_torrent("example.torrent");
client.start();  // non-blocking, returns immediately

// Drive the loop in your own thread
loop.loop();
```

### 6.4 Advanced: Direct NAT Manager Access

```cpp
auto client = yuan::net::bit_torrent::BitTorrentClient();
client.load_torrent("example.torrent");
client.start();

// Access NAT subsystems after start
auto *nat = client.get_nat_manager();
if (nat) {
    printf("Listening: %s\n", nat->is_listening() ? "yes" : "no");
    printf("UPnP mapped: %s\n", nat->is_upnp_mapped() ? "yes" : "no");
    printf("DHT running: %s\n", nat->is_dht_running() ? "yes" : "no");
    printf("External IP: %s:%d\n",
           nat->get_external_ip().c_str(), nat->get_external_port());

    // Access PEX manager for a specific peer
    auto *pex = nat->get_pex_manager();
    printf("PEX known peers: %zu\n", pex->peer_count());
}
```

### 6.5 Tracker-Only Usage

```cpp
// HTTP tracker
yuan::net::bit_torrent::HttpTracker http_tracker;
yuan::net::bit_torrent::TrackerResponse resp;
http_tracker.announce("http://tracker.example.com/announce",
                      meta, 6881, 0, 0, -1, &resp);
for (const auto& peer : resp.peers_)
    printf("Peer: %s:%d\n", peer.ip_.c_str(), peer.port_);

// UDP tracker
yuan::net::bit_torrent::UdpTracker udp_tracker;
yuan::net::bit_torrent::UdpTrackerResponse udp_resp;
udp_tracker.announce("tracker.example.com", 6969,
                     meta, 6881, 0, 0, -1, &udp_resp);
```

### 6.6 Inbound Peer Connection

```cpp
// PeerListener handles this automatically via NatManager.
// For standalone use:
auto *listener = new yuan::net::bit_torrent::PeerListener();
listener->set_new_peer_callback([](yuan::net::bit_torrent::PeerConnection *peer) {
    // Handle inbound peer
});
listener->start(nat_cfg, meta, peer_id, pieces_have, loop, timer_mgr);
```

### 6.7 uTP Connection

```cpp
// Via NatManager (automatic when both peers support uTP):
auto *utp_mgr = nat->get_utp_manager();  // if UtpManager exposed
auto *conn = utp_mgr->connect("1.2.3.4", 6889, info_hash, peer_id);
conn->set_data_handler([](const uint8_t *data, size_t len) {
    // Process BT protocol data over uTP
});
conn->send_data(handshake_data.data(), handshake_data.size());
```

### 6.8 DHT Peer Lookup

```cpp
// DHT is started automatically by NatManager.
// To manually look up peers:
auto *dht = nat->get_dht_node();  // if exposed
dht->get_peers(info_hash, [](const std::vector<PeerAddress> &peers) {
    for (const auto &p : peers)
        printf("DHT peer: %s:%d\n", p.ip_.c_str(), p.port_);
});
```

## 7. Build

```bash
# From project root
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target BitTorrentProto

# Build tests
cmake --build . --target test_bit_torrent
```

## 8. Future Enhancements

- [ ] **BEP 9 Magnet Links**: `magnet:?xt=urn:btih:<info_hash>` support (needs ut_metadata)
- [ ] **BEP 6 Fast Extensions**: `have_all`, `have_none`, `suggest`, `reject`
- [ ] **Multi-threaded Event Loop**: One loop per CPU core
- [ ] **Piece Selection**: Rarest-first algorithm with priority
- [ ] **Upload Support**: Seeding with choking/unchoking algorithm
- [ ] **SSL Tracker**: Integration with `HttpClient` + `SSLModule`
- [ ] **Merkle Trees**: BEP 30 for reduced metadata ( Bramble )
- [ ] **IPv6 Support**: Dual-stack listening and peer connections
- [ ] **uTP + BT Protocol Integration**: Full BT handshake over uTP transport
- [ ] **DHT Persistent Storage**: Save routing table to disk across sessions
- [ ] **Multi-Torrent**: Support downloading multiple torrents simultaneously
