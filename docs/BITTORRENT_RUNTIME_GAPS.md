# BitTorrent Runtime Gaps

## Current Judgment

`BitTorrentProto` is buildable and its unit-style tests pass, but it should **not** yet be treated as a complete, production-ready BitTorrent downloader/uploader.

Current state:

- metadata parsing works
- tracker HTTP/UDP announce works at protocol level
- TCP peer wire handshake and basic message parsing exist
- NAT/uTP/DHT/PEX subsystems exist structurally
- `BitTorrentClient` startup/runtime is now more modular
- local downloader/seeder end-to-end verification now exists and passes through the project test target

But the full download/upload pipeline is still incomplete.

## Verified Missing Pieces

### 1. Piece completion pipeline is wired, but not yet production-grade

Evidence:

- [bit_torrent_client.cpp](/d:/code/src/vs/webserver/protocol/bit_torrent/src/bit_torrent_client.cpp) writes incoming piece data in `on_piece_data(...)`
- the runtime now has a minimal path that detects expected piece length, verifies the piece hash, marks the piece completed, updates stats, and broadcasts `have`
- pending block requests from a lost peer can now be requeued back into `PieceDownloadState` instead of being silently dropped
- local end-to-end download now verifies that a downloader can fetch a real file from a local seeder through a local tracker, commit it to disk, and finish cleanly
- startup now also restores fully written and hash-valid `.partial.*` piece files into committed output, so one practical resume gap is closed
- however, this is still not a full production-grade piece/block scheduler and file assembly pipeline

Impact:

- completed pieces can now enter a validated finished state
- local single-seeder download behavior is now proven by executable coverage
- but multi-peer block scheduling, resume hardening, and broader torrent-runtime validation are still incomplete

### 2. Upload/seeding path is present, but policy is still minimal

Evidence:

- [peer_connection.cpp](/d:/code/src/vs/webserver/protocol/bit_torrent/src/peer_wire/peer_connection.cpp) now handles incoming `request` messages and can send `piece` responses through an injected piece-block provider
- [PieceStorage](/d:/code/src/vs/webserver/protocol/bit_torrent/src/storage/piece_storage.cpp) can read committed file blocks for completed local pieces
- upload byte accounting now flows back into [DownloadStatsTracker](/d:/code/src/vs/webserver/protocol/bit_torrent/src/stats/download_stats_tracker.cpp), so tracker announce context is no longer upload-blind
- tracker announce lifecycle is now less placeholder: started/completed/stopped semantics are available instead of every announce being treated as a started event
- local seeding behavior is now covered by the downloader/seeder e2e path
- however, upload policy is still minimal and does not yet implement fuller choke/unchoke policy, rate limiting, fairness, or broader long-running seeding validation

Impact:

- current implementation has a real first upload path
- it still should not yet be treated as a complete uploader/seeder

### 3. Piece scheduling is still simplified

Evidence:

- request selection has moved out of `PeerConnection` and into `PieceDownloadState`, so peer transport is no longer responsible for scheduling decisions
- the current scheduler now tracks per-piece request offsets, suppresses duplicate in-flight block requests, can continue requesting subsequent blocks from the same peer after a block arrives, and can requeue pending blocks lost with a failed peer
- it now includes a lightweight rarest-first preference, duplicate in-flight suppression, lost-request requeue, and a small per-peer request window
- it still does not implement stronger multi-peer scheduling, peer-quality-aware scheduling, endgame behavior, or a fuller retry/backoff policy

Impact:

- download behavior is now beyond toy protocol coverage and has local e2e proof
- the strategy is still too simple for real torrent workloads

### 4. uTP transport is not fully integrated into BitTorrent peer protocol

Evidence:

- [nat_manager.cpp](/d:/code/src/vs/webserver/protocol/bit_torrent/src/nat/nat_manager.cpp) explicitly says uTP currently only completes the transport layer
- comments note full BT handshake/message processing over uTP is still placeholder
- [DESIGN.md](/d:/code/src/vs/webserver/protocol/bit_torrent/DESIGN.md) lists `uTP + BT Protocol Integration` as unfinished

Impact:

- uTP cannot yet be treated as a fully working alternative peer transport

### 5. End-to-end verification exists locally, but breadth is still limited

Evidence:

- [test_bit_torrent.cpp](/d:/code/src/vs/webserver/test/test_bit_torrent.cpp) is primarily unit/protocol/config coverage
- it now also contains a real local downloader/seeder e2e scenario:
  one local tracker, one local seeder, one local downloader, and final file verification
- that local e2e path has been rerun successfully multiple times through `test_bit_torrent`
- it still does not verify real multi-peer torrent download completion
- it still does not verify broader seeding/uploader behavior under longer-lived runtime conditions

Impact:

- local runtime capability is now proven by an executable e2e torrent scenario
- broader runtime readiness is still not proven

## What This Means Today

Today `BitTorrentProto` should be considered:

- architecturally migrated enough to stay on the new mainline
- protocol-rich enough to continue evolving
- locally e2e-capable for a minimal downloader/seeder path
- **not yet complete enough to claim real-world download/upload readiness**

## Recommended Follow-Up After Mainline Work

When mainline work returns to BitTorrent completion, the recommended order is:

1. expand `PieceDownloadState` from the current small per-peer request-window model into stronger multi-peer scheduling
2. harden final file assembly and resume policy beyond the current local e2e path
3. harden upload/seeding policy: choke/unchoke, rate limits, and longer-lived seeding behavior
4. complete uTP + BT protocol integration
5. add broader e2e torrent scenarios:
   multi-peer download, resume after restart, and longer-lived seeding/upload validation
