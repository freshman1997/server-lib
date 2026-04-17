#include "bit_torrent_client.h"
#include "runtime/download_runtime_coordinator.h"
#include "storage/piece_storage.h"
#include "utils.h"
#include "peer_wire/peer_connection.h"
#include "timer/timer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    namespace
    {

        size_t compute_max_active_pieces(const DownloadRuntimeCoordinator *runtime_coordinator)
        {
            if (!runtime_coordinator) {
                return 2;
            }

            const auto active_peers = runtime_coordinator->get_active_peers().size();
            if (active_peers <= 1) {
                return 2;
            }

            return std::min<size_t>(8, active_peers);
        }

    } // namespace

    BitTorrentClient::BitTorrentClient()
        : piece_storage_(std::make_unique<PieceStorage>()),
          runtime_coordinator_(std::make_unique<DownloadRuntimeCoordinator>()),
          running_(false)
    {
    }

    BitTorrentClient::~BitTorrentClient()
    {
        stop();
    }

    NatManager *BitTorrentClient::get_nat_manager()
    {
        return runtime_coordinator_ ? runtime_coordinator_->get_nat_manager() : nullptr;
    }

    bool BitTorrentClient::load_torrent(const std::string & file_path)
    {
        meta_ = TorrentMeta::parse_file(file_path);
        if (meta_.info_hash_.empty())
            return false;

        piece_state_.reset(meta_.info.piece_count(), meta_.info.total_length_, meta_.info.piece_length_);
        stats_tracker_.reset(meta_.info.total_length_, meta_.info.piece_count());
        return true;
    }

    bool BitTorrentClient::load_torrent_data(const std::string & torrent_data)
    {
        meta_ = TorrentMeta::parse(torrent_data);
        if (meta_.info_hash_.empty())
            return false;

        piece_state_.reset(meta_.info.piece_count(), meta_.info.total_length_, meta_.info.piece_length_);
        stats_tracker_.reset(meta_.info.total_length_, meta_.info.piece_count());
        return true;
    }

    void BitTorrentClient::preload_existing_pieces()
    {
        if (!piece_storage_) {
            return;
        }

        auto mark_preloaded_piece = [this](uint32_t piece_index) {
        if (!piece_state_.mark_piece_completed(piece_index))
        {
            return;
        }

        const int64_t piece_offset = static_cast<int64_t>(piece_index) * meta_.info.piece_length_;
        const auto piece_size = static_cast<uint32_t>(
            std::min<int64_t>(meta_.info.piece_length_, meta_.info.total_length_ - piece_offset));
        stats_tracker_.add_downloaded_bytes(piece_size);
        stats_tracker_.set_piece_completed(piece_index);
        };

        for (const auto piece_index : piece_storage_->restore_verified_partial_pieces()) {
            mark_preloaded_piece(piece_index);
        }

        for (const auto piece_index : piece_storage_->scan_committed_pieces()) {
            mark_preloaded_piece(piece_index);
        }
    }

    void BitTorrentClient::start_download_runtime()
    {
        if (!runtime_coordinator_) {
            runtime_coordinator_ = std::make_unique<DownloadRuntimeCoordinator>();
        }

        DownloadRuntimeConfig runtime_config;
        runtime_config.runtime_ = runtime_;
        runtime_config.meta_ = &meta_;
        runtime_config.peer_id_ = &peer_id_;
        runtime_config.pieces_have_ = &piece_state_.pieces_have();
        runtime_config.listen_port_ = &listen_port_;
        runtime_config.max_peers_ = max_peers_;
        runtime_config.nat_config_ = nat_config_;
        runtime_config.stats_tracker_ = &stats_tracker_;
        runtime_config.piece_data_handler_ = [this](PeerConnection *peer, uint32_t piece_index, uint32_t offset,
                                                    const uint8_t *data, uint32_t length) {
        on_piece_data(peer, piece_index, offset, data, length);
        };
        runtime_config.piece_request_handler_ = [this](uint32_t piece_index, uint32_t offset,
                                                       uint32_t length, std::vector<uint8_t> &out) {
        return on_piece_request(piece_index, offset, length, out);
        };
        runtime_config.piece_served_handler_ = [this](uint32_t piece_index, uint32_t offset, uint32_t length) {
        on_piece_served(piece_index, offset, length);
        };
        runtime_config.peer_ready_handler_ = [this](PeerConnection *peer) {
        on_peer_connected(peer);
        };
        runtime_config.peer_lost_handler_ = [this](const std::vector<PieceBlockRequest> &requests) {
        on_peer_requests_lost(requests);
        };
        runtime_coordinator_->configure(std::move(runtime_config));
        runtime_coordinator_->start(!owned_runtime_);

        if (runtime_) {
            unchoke_timer_ = runtime_->schedule_periodic(
                10000, 10000, [this]() { perform_choking_round(); }, -1);
        }
    }

    void BitTorrentClient::stop_download_runtime()
    {
        if (unchoke_timer_) {
            unchoke_timer_->cancel();
            unchoke_timer_ = nullptr;
        }
        if (runtime_coordinator_) {
            runtime_coordinator_->stop();
        }
        if (piece_storage_) {
            piece_storage_->flush_all();
            piece_storage_->close_all();
        }
    }

    bool BitTorrentClient::start()
    {
        if (meta_.info_hash_.empty() || running_.load())
            return false;

        peer_id_ = generate_peer_id();
        running_ = true;

        if (!runtime_) {
            owned_runtime_ = std::make_unique<NetworkRuntime>();
            runtime_ = owned_runtime_.get();
        }

        if (save_path_.empty())
            save_path_ = ".";
        if (piece_storage_) {
            piece_storage_->configure(&meta_, save_path_);
            preload_existing_pieces();
        }

        start_download_runtime();

        if (owned_runtime_) {
            owned_runtime_->run();
        }

        return true;
    }

    void BitTorrentClient::stop()
    {
        if (!running_.exchange(false))
            return;

        stop_download_runtime();

        if (owned_runtime_) {
            owned_runtime_->stop();
            owned_runtime_.reset();
            runtime_ = nullptr;
        }
    }

    void BitTorrentClient::on_peer_connected(PeerConnection * peer)
    {
        if (peer->get_state() == PeerConnection::State::connected) {
            PeerState our_state;
            our_state.pieces = piece_state_.pieces_have();
            auto bf = our_state.to_bitfield();
            if (!bf.empty())
                peer->send_bitfield(bf);

            peer->send_choke();

            if (!piece_state_.is_complete()) {
                peer->send_interested();
                request_next_block(peer);
            } else {
                peer->send_not_interested();
            }
        }
    }

    void BitTorrentClient::on_piece_data(PeerConnection * peer, uint32_t piece_index, uint32_t offset,
                                         const uint8_t * data, uint32_t length)
    {
        if (piece_storage_ &&
            piece_storage_->write_piece(static_cast<int32_t>(piece_index), offset, data, length)) {
            const auto accounted = piece_state_.mark_block_received(piece_index, offset, length);
            if (accounted > 0) {
                stats_tracker_.add_downloaded_bytes(accounted);
            }

            if (piece_storage_->is_piece_complete(static_cast<int32_t>(piece_index)) &&
                piece_storage_->verify_piece(static_cast<int32_t>(piece_index)) &&
                piece_storage_->commit_piece(static_cast<int32_t>(piece_index)) &&
                piece_state_.mark_piece_completed(piece_index)) {
                stats_tracker_.set_piece_completed(piece_index);

                if (piece_state_.is_endgame() || piece_state_.is_complete()) {
                    for (auto *active_peer : runtime_coordinator_->get_active_peers()) {
                        if (active_peer && active_peer != peer) {
                            active_peer->send_cancel(piece_index, offset, length);
                        }
                    }
                }

                if (runtime_coordinator_) {
                    runtime_coordinator_->broadcast_have(piece_index);
                    if (piece_state_.is_complete()) {
                        runtime_coordinator_->announce_now();
                        for (auto *active_peer : runtime_coordinator_->get_active_peers()) {
                            if (active_peer) {
                                active_peer->send_not_interested();
                            }
                        }
                    }
                }
                request_next_block(peer);
                return;
            }

            if (piece_storage_->is_piece_complete(static_cast<int32_t>(piece_index))) {
                piece_state_.mark_piece_failed(piece_index);
                return;
            }

            request_next_block(peer);
        }
    }

    void BitTorrentClient::request_next_block(PeerConnection * peer)
    {
        if (!peer || !peer->can_download()) {
            return;
        }

        const auto piece_availability = build_piece_availability();
        const auto max_active_pieces = compute_max_active_pieces(runtime_coordinator_.get());
        while (peer->pending_request_count() < peer->request_window_size()) {
            PieceBlockRequest request;
            if (!piece_state_.select_next_request(peer->get_peer_state().pieces,
                                                  piece_availability.empty() ? nullptr : &piece_availability,
                                                  16 * 1024,
                                                  max_active_pieces,
                                                  request)) {
                break;
            }
            peer->send_request(request.piece_index_, request.offset_, request.length_);
        }
    }

    void BitTorrentClient::on_peer_requests_lost(const std::vector<PieceBlockRequest> & requests)
    {
        for (const auto &request : requests) {
            piece_state_.requeue_block(request.piece_index_, request.offset_, request.length_);
        }

        if (!runtime_coordinator_) {
            return;
        }

        for (auto *peer : runtime_coordinator_->get_active_peers()) {
            request_next_block(peer);
        }
    }

    bool BitTorrentClient::on_piece_request(uint32_t piece_index, uint32_t offset,
                                            uint32_t length, std::vector<uint8_t> & out)
    {
        return piece_storage_ && piece_state_.pieces_have().size() > piece_index &&
               piece_state_.pieces_have()[piece_index] &&
               piece_storage_->read_block(piece_index, offset, length, out);
    }

    void BitTorrentClient::on_piece_served(uint32_t piece_index, uint32_t offset, uint32_t length)
    {
        (void)piece_index;
        (void)offset;
        stats_tracker_.add_uploaded_bytes(length);
    }

    std::vector<uint32_t> BitTorrentClient::build_piece_availability() const
    {
        if (!runtime_coordinator_) {
            return {};
        }

        std::vector<uint32_t> availability(meta_.info.piece_count(), 0);
        for (const auto *peer : runtime_coordinator_->get_active_peers()) {
            if (!peer) {
                continue;
            }

            const auto &pieces = peer->get_peer_state().pieces;
            const size_t count = std::min(pieces.size(), availability.size());
            for (size_t i = 0; i < count; ++i) {
                if (pieces[i]) {
                    ++availability[i];
                }
            }
        }
        return availability;
    }

    int32_t BitTorrentClient::get_peer_count() const
    {
        return runtime_coordinator_ ? runtime_coordinator_->get_peer_count() : 0;
    }

    void BitTorrentClient::perform_choking_round()
    {
        if (!runtime_coordinator_) {
            return;
        }

        auto peers = runtime_coordinator_->get_active_peers();

        std::vector<PeerConnection *> interested;
        for (auto *peer : peers) {
            if (peer && peer->get_peer_state().peer_interested) {
                interested.push_back(peer);
            }
        }

        std::sort(interested.begin(), interested.end(),
                  [](PeerConnection *a, PeerConnection *b) {
                return a->get_peer_ip() < b->get_peer_ip();
        });

        int32_t slots_remaining = upload_slots_;
        for (auto *peer : peers) {
            if (!peer)
                continue;

            bool should_unchoke = false;
            if (slots_remaining > 0) {
                auto it = std::find(interested.begin(), interested.end(), peer);
                if (it != interested.end()) {
                    should_unchoke = true;
                    --slots_remaining;
                }
            }

            auto &state = peer->mutable_peer_state();
            if (should_unchoke && state.am_choking) {
                peer->send_unchoke();
            } else if (!should_unchoke && !state.am_choking) {
                peer->send_choke();
            }
        }
    }

} // namespace yuan::net::bit_torrent
