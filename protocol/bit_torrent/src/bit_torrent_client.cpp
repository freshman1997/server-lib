#include "bit_torrent_client.h"
#include "runtime/download_runtime_coordinator.h"
#include "storage/piece_storage.h"
#include "utils.h"
#include "magnet_uri.h"
#include "structure/bencoding.h"
#include "peer_wire/peer_connection.h"
#include "timer/timer.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace yuan::net::bit_torrent
{

    namespace
    {
        template <typename T>
        T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }

        template <typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

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

        uint64_t monotonic_now_ms()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
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
        torrent_completed_emitted_ = false;
        download_budget_bytes_ = 0.0;
        upload_budget_bytes_ = 0.0;
        bandwidth_last_refill_ms_ = monotonic_now_ms();
        metadata_mode_ = false;
        magnet_tracker_urls_.clear();
        return true;
    }

    bool BitTorrentClient::load_torrent_data(const std::string & torrent_data)
    {
        meta_ = TorrentMeta::parse(torrent_data);
        if (meta_.info_hash_.empty())
            return false;

        piece_state_.reset(meta_.info.piece_count(), meta_.info.total_length_, meta_.info.piece_length_);
        stats_tracker_.reset(meta_.info.total_length_, meta_.info.piece_count());
        torrent_completed_emitted_ = false;
        download_budget_bytes_ = 0.0;
        upload_budget_bytes_ = 0.0;
        bandwidth_last_refill_ms_ = monotonic_now_ms();
        metadata_mode_ = false;
        magnet_tracker_urls_.clear();
        return true;
    }

    bool BitTorrentClient::load_magnet(const std::string & magnet_uri)
    {
        auto parsed = MagnetUri::parse(magnet_uri);
        if (!parsed.valid)
            return false;

        meta_ = TorrentMeta();
        meta_.info_hash_ = parsed.info_hash;
        meta_.info_hash_hex_ = parsed.info_hash_hex;
        meta_.info.name_ = parsed.display_name;

        if (!parsed.tracker_urls.empty())
        {
            meta_.announce_ = parsed.tracker_urls[0];
            for (const auto &url : parsed.tracker_urls)
            {
                meta_.announce_list_.push_back({url});
            }
        }

        magnet_tracker_urls_ = parsed.tracker_urls;
        metadata_mode_ = true;

        piece_state_.reset(0, 0, 0);
        stats_tracker_.reset(0, 0);
        torrent_completed_emitted_ = false;
        download_budget_bytes_ = 0.0;
        upload_budget_bytes_ = 0.0;
        bandwidth_last_refill_ms_ = monotonic_now_ms();
        return true;
    }

    void BitTorrentClient::on_metadata_received(const std::vector<uint8_t> & metadata)
    {
        if (!metadata_mode_)
            return;

        std::string info_dict(reinterpret_cast<const char *>(metadata.data()), metadata.size());
        auto parsed = BencodingDataConverter::parse(info_dict);
        if (!parsed || parsed->type_ != DataType::dictionary_)
            return;

        auto *info_dict_data = static_cast<DicttionaryData *>(parsed);

        if (auto *nv = info_dict_data->get_val("name"); nv && nv->type_ == DataType::string_)
            meta_.info.name_ = dynamic_cast<StringData *>(nv)->get_data();

        if (auto *nv = info_dict_data->get_val("piece length"); nv && nv->type_ == DataType::integer_)
            meta_.info.piece_length_ = dynamic_cast<IntegerData *>(nv)->get_data();

        if (auto *nv = info_dict_data->get_val("pieces"); nv && nv->type_ == DataType::string_)
            meta_.info.pieces_ = dynamic_cast<StringData *>(nv)->get_data();

        if (auto *nv = info_dict_data->get_val("private"); nv && nv->type_ == DataType::integer_)
            meta_.info.private_ = dynamic_cast<IntegerData *>(nv)->get_data() != 0;

        if (auto *nv = info_dict_data->get_val("length"); nv && nv->type_ == DataType::integer_)
            meta_.info.total_length_ = dynamic_cast<IntegerData *>(nv)->get_data();

        if (auto *nv = info_dict_data->get_val("files"); nv && nv->type_ == DataType::list_) {
            auto *files_list = dynamic_cast<Listdata *>(nv);
            int64_t offset = 0;
            for (size_t i = 0; i < files_list->get_data().size(); i++) {
                auto *file_node = files_list->get_data()[i];
                if (!file_node || file_node->type_ != DataType::dictionary_)
                    continue;
                auto *file_dict = dynamic_cast<DicttionaryData *>(file_node);

                TorrentFile tf;
                tf.offset_ = offset;

                if (auto *lv = file_dict->get_val("length"); lv && lv->type_ == DataType::integer_)
                    tf.length_ = dynamic_cast<IntegerData *>(lv)->get_data();

                if (auto *lv = file_dict->get_val("path"); lv && lv->type_ == DataType::list_) {
                    auto *path_list = dynamic_cast<Listdata *>(lv);
                    for (size_t j = 0; j < path_list->get_data().size(); j++) {
                        auto *p_node = path_list->get_data()[j];
                        if (p_node && p_node->type_ == DataType::string_)
                            tf.path_.push_back(dynamic_cast<StringData *>(p_node)->get_data());
                    }
                }

                meta_.info.files_.push_back(tf);
                offset += tf.length_;
            }
            if (meta_.info.total_length_ == 0)
                meta_.info.total_length_ = offset;
        }

        delete parsed;

        if (meta_.info.piece_count() <= 0 || meta_.info.total_length_ <= 0)
            return;

        piece_state_.reset(meta_.info.piece_count(), meta_.info.total_length_, meta_.info.piece_length_);
        stats_tracker_.reset(meta_.info.total_length_, meta_.info.piece_count());

        metadata_mode_ = false;

        if (save_path_.empty())
            save_path_ = ".";
        if (piece_storage_) {
            piece_storage_->configure(&meta_, save_path_);
            preload_existing_pieces();
        }

        if (runtime_coordinator_) {
            for (const auto &active_peer : runtime_coordinator_->get_active_peers()) {
                if (!active_peer)
                    continue;

                PeerState our_state;
                our_state.pieces = piece_state_.pieces_have();
                auto bf = our_state.to_bitfield();
                if (!bf.empty())
                    active_peer->send_bitfield(bf);

                if (!piece_state_.is_complete()) {
                    active_peer->send_interested();
                    request_next_block(ptr_of(active_peer));
                }
            }

            runtime_coordinator_->announce_now();
        }

        if (metadata_received_callback_) {
            metadata_received_callback_();
        }
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
        if (piece_completed_callback_) {
            piece_completed_callback_(piece_index, piece_size);
        }
        };

        for (const auto piece_index : piece_storage_->restore_verified_partial_pieces()) {
            mark_preloaded_piece(piece_index);
        }

        for (const auto piece_index : piece_storage_->scan_committed_pieces()) {
            mark_preloaded_piece(piece_index);
        }
    }

    bool BitTorrentClient::start_download_runtime()
    {
        if (download_limit_kbps_ > 0) {
            download_budget_bytes_ = static_cast<double>(download_limit_kbps_) * 1024.0;
        }
        if (upload_limit_kbps_ > 0) {
            upload_budget_bytes_ = static_cast<double>(upload_limit_kbps_) * 1024.0;
        }

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
        runtime_config.peer_unchoke_handler_ = [this](PeerConnection *peer) {
        request_next_block(peer);
        };
        runtime_config.peer_reject_handler_ = [this](PeerConnection *peer, uint32_t piece, uint32_t offset, uint32_t length) {
        piece_state_.requeue_block(piece, offset, length);
        request_next_block(peer);
        };
        runtime_config.peer_lost_handler_ = [this](const std::vector<PieceBlockRequest> &requests) {
        on_peer_requests_lost(requests);
        };
        runtime_coordinator_->configure(std::move(runtime_config));
        if (!runtime_coordinator_->start(!owned_runtime_)) {
            return false;
        }

        auto *nat = runtime_coordinator_->get_nat_manager();
        if (nat) {
            auto *mm = nat->get_metadata_manager();
            if (mm) {
                mm->set_metadata_complete_callback([this](const std::vector<uint8_t> &metadata) {
                    on_metadata_received(metadata);
                });
            }
        }

        if (runtime_) {
            unchoke_timer_ = runtime_->schedule_periodic(
                10000, 10000, [this]() { perform_choking_round(); }, -1);
        }

        return true;
    }

    void BitTorrentClient::stop_download_runtime()
    {
        if (unchoke_timer_) {
            unchoke_timer_->cancel();
            unchoke_timer_ = nullptr;
        }
        if (runtime_coordinator_) {
            runtime_coordinator_->stop();
            runtime_coordinator_.reset();
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
        torrent_completed_emitted_ = false;

        if (!runtime_) {
            owned_runtime_ = std::make_unique<NetworkRuntime>();
            runtime_ = ptr_of(owned_runtime_);
        }

        download_budget_bytes_ = 0.0;
        upload_budget_bytes_ = 0.0;
        bandwidth_last_refill_ms_ = monotonic_now_ms();

        if (save_path_.empty())
            save_path_ = ".";
        if (piece_storage_) {
            piece_storage_->configure(&meta_, save_path_);
            preload_existing_pieces();
        }

        if (!start_download_runtime()) {
            running_ = false;
            if (owned_runtime_) {
                owned_runtime_->stop();
                owned_runtime_.reset();
                runtime_ = nullptr;
            }
            return false;
        }

        emit_torrent_completed_once();

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

    void BitTorrentClient::clear_torrent()
    {
        if (running_.load()) {
            stop();
        }

        meta_ = TorrentMeta{};
        piece_state_.reset(0, 0, 0);
        stats_tracker_.reset(0, 0);
        torrent_completed_emitted_ = false;
        download_budget_bytes_ = 0.0;
        upload_budget_bytes_ = 0.0;
        bandwidth_last_refill_ms_ = monotonic_now_ms();
        metadata_mode_ = false;
        magnet_tracker_urls_.clear();
        if (piece_storage_) {
            piece_storage_->flush_all();
            piece_storage_->close_all();
        }
    }

    void BitTorrentClient::on_peer_connected(PeerConnection * peer)
    {
        if (peer->get_state() == PeerConnection::State::connected) {
            if (peer_connected_callback_) {
                peer_connected_callback_(peer->get_peer_ip(), peer->get_peer_port(), peer->get_peer_id());
            }

            if (metadata_mode_) {
                peer->send_interested();
                return;
            }

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

            if (piece_state_.is_endgame() && runtime_coordinator_) {
                for (const auto &active_peer : runtime_coordinator_->get_active_peers()) {
                    if (active_peer && ptr_of(active_peer) != peer) {
                        active_peer->send_cancel(piece_index, offset, length);
                    }
                }
            }

            if (piece_storage_->is_piece_complete(static_cast<int32_t>(piece_index)) &&
                piece_storage_->verify_piece(static_cast<int32_t>(piece_index)) &&
                piece_storage_->commit_piece(static_cast<int32_t>(piece_index)) &&
                piece_state_.mark_piece_completed(piece_index)) {
                const int64_t piece_offset = static_cast<int64_t>(piece_index) * meta_.info.piece_length_;
                const auto piece_size = static_cast<uint32_t>(
                    std::min<int64_t>(meta_.info.piece_length_, meta_.info.total_length_ - piece_offset));

                stats_tracker_.set_piece_completed(piece_index);
                if (piece_completed_callback_) {
                    piece_completed_callback_(piece_index, piece_size);
                }

                if (piece_state_.is_endgame() || piece_state_.is_complete()) {
                    for (const auto &active_peer : runtime_coordinator_->get_active_peers()) {
                        if (active_peer && ptr_of(active_peer) != peer) {
                            active_peer->send_cancel(piece_index, offset, length);
                        }
                    }
                }

                if (runtime_coordinator_) {
                    runtime_coordinator_->broadcast_have(piece_index);
                    if (piece_state_.is_complete()) {
                        emit_torrent_completed_once();
                        runtime_coordinator_->announce_now();
                        for (const auto &active_peer : runtime_coordinator_->get_active_peers()) {
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
                piece_storage_->discard_piece(static_cast<int32_t>(piece_index));
                piece_state_.mark_piece_failed(piece_index);
                request_next_block(peer);
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

        refill_bandwidth_budget();

        const auto peers = runtime_coordinator_ ? runtime_coordinator_->get_active_peers() : std::vector<std::shared_ptr<PeerConnection> >{};
        const auto piece_availability = build_piece_availability(peers);
        const auto max_active_pieces = compute_max_active_pieces(ptr_of(runtime_coordinator_));
        const auto now_ms = monotonic_now_ms();
        while (peer->pending_request_count() < peer->request_window_size()) {
            if (download_limit_kbps_ > 0 && download_budget_bytes_ < 1024.0) {
                break;
            }

            PieceBlockRequest request;
            if (!piece_state_.select_next_request(peer->get_peer_state().pieces,
                                                  piece_availability.empty() ? nullptr : &piece_availability,
                                                  16 * 1024,
                                                  max_active_pieces,
                                                  now_ms,
                                                  request)) {
                if (!piece_state_.select_endgame_request(peer->get_peer_state().pieces, now_ms, request)) {
                    break;
                }
            }

            if (!consume_download_budget(request.length_)) {
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

        for (const auto &peer : runtime_coordinator_->get_active_peers()) {
            request_next_block(ptr_of(peer));
        }
    }

    bool BitTorrentClient::on_piece_request(uint32_t piece_index, uint32_t offset,
                                            uint32_t length, std::vector<uint8_t> & out)
    {
        refill_bandwidth_budget();
        if (!consume_upload_budget(length)) {
            return false;
        }

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

    std::vector<uint32_t> BitTorrentClient::build_piece_availability(const std::vector<std::shared_ptr<PeerConnection> > &peers) const
    {
        if (peers.empty()) {
            return {};
        }

        std::vector<uint32_t> availability(meta_.info.piece_count(), 0);
        for (const auto &peer : peers) {
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

    std::vector<std::shared_ptr<PeerConnection>> BitTorrentClient::get_active_peers() const
    {
        return runtime_coordinator_ ? runtime_coordinator_->get_active_peers() : std::vector<std::shared_ptr<PeerConnection>>{};
    }

    bool BitTorrentClient::emit_torrent_completed_once()
    {
        if (!piece_state_.is_complete() || torrent_completed_emitted_) {
            return false;
        }

        torrent_completed_emitted_ = true;
        if (torrent_completed_callback_) {
            torrent_completed_callback_();
        }
        return true;
    }

    void BitTorrentClient::refill_bandwidth_budget()
    {
        const auto now = monotonic_now_ms();
        if (bandwidth_last_refill_ms_ == 0) {
            bandwidth_last_refill_ms_ = now;
            return;
        }

        const auto elapsed_ms = now > bandwidth_last_refill_ms_ ? (now - bandwidth_last_refill_ms_) : 0;
        bandwidth_last_refill_ms_ = now;
        if (elapsed_ms == 0) {
            return;
        }

        const double elapsed_sec = static_cast<double>(elapsed_ms) / 1000.0;
        if (download_limit_kbps_ > 0) {
            const double refill = static_cast<double>(download_limit_kbps_) * 1024.0 * elapsed_sec;
            const double cap = static_cast<double>(download_limit_kbps_) * 1024.0 * 2.0;
            download_budget_bytes_ = std::min(cap, download_budget_bytes_ + refill);
        } else {
            download_budget_bytes_ = 0.0;
        }

        if (upload_limit_kbps_ > 0) {
            const double refill = static_cast<double>(upload_limit_kbps_) * 1024.0 * elapsed_sec;
            const double cap = static_cast<double>(upload_limit_kbps_) * 1024.0 * 2.0;
            upload_budget_bytes_ = std::min(cap, upload_budget_bytes_ + refill);
        } else {
            upload_budget_bytes_ = 0.0;
        }
    }

    bool BitTorrentClient::consume_download_budget(uint32_t bytes)
    {
        if (download_limit_kbps_ <= 0) {
            return true;
        }
        if (download_budget_bytes_ < static_cast<double>(bytes)) {
            return false;
        }
        download_budget_bytes_ -= static_cast<double>(bytes);
        return true;
    }

    bool BitTorrentClient::consume_upload_budget(uint32_t bytes)
    {
        if (upload_limit_kbps_ <= 0) {
            return true;
        }
        if (upload_budget_bytes_ < static_cast<double>(bytes)) {
            return false;
        }
        upload_budget_bytes_ -= static_cast<double>(bytes);
        return true;
    }

    void BitTorrentClient::perform_choking_round()
    {
        if (!runtime_coordinator_) {
            return;
        }

        auto now_ms = monotonic_now_ms();
        auto peers = runtime_coordinator_->get_active_peers();

        for (const auto &peer : peers) {
            if (peer) {
                peer->update_rates(now_ms);
            }
        }

        static constexpr uint64_t REQUEST_TIMEOUT_MS = 30000;
        auto timed_out = piece_state_.timeout_inflight_requests(now_ms, REQUEST_TIMEOUT_MS);
        if (!timed_out.empty()) {
            for (const auto &peer : peers) {
                if (peer && peer->can_download()) {
                    request_next_block(ptr_of(peer));
                }
            }
        }

        std::vector<std::shared_ptr<PeerConnection>> interested;
        for (const auto &peer : peers) {
            if (peer && peer->get_peer_state().peer_interested) {
                interested.push_back(peer);
            }
        }

        std::sort(interested.begin(), interested.end(),
                  [](const std::shared_ptr<PeerConnection> &a, const std::shared_ptr<PeerConnection> &b) {
                      return a->download_rate() > b->download_rate();
                  });

        std::vector<PeerConnection *> to_unchoke;

        int32_t regular_slots = upload_slots_;
        for (const auto &peer : interested) {
            if (static_cast<int32_t>(to_unchoke.size()) >= regular_slots)
                break;
            if (!peer->is_snubbed()) {
                to_unchoke.push_back(peer.get());
            }
        }

        optimistic_unchoke_counter_++;
        if (optimistic_unchoke_counter_ >= OPTIMISTIC_UNCHOKE_INTERVAL) {
            optimistic_unchoke_counter_ = 0;

            std::vector<PeerConnection *> candidates;
            for (const auto &peer : interested) {
                if (std::find(to_unchoke.begin(), to_unchoke.end(), peer.get()) == to_unchoke.end()) {
                    candidates.push_back(peer.get());
                }
            }

            if (!candidates.empty()) {
                size_t idx = static_cast<size_t>(now_ms) % candidates.size();
                to_unchoke.push_back(candidates[idx]);
                optimistic_unchoke_index_ = static_cast<int32_t>(idx);
            }
        }

        for (const auto &peer : peers) {
            if (!peer)
                continue;

            auto &state = peer->mutable_peer_state();
            bool should_unchoke = std::find(to_unchoke.begin(), to_unchoke.end(), peer.get()) != to_unchoke.end();

            if (should_unchoke && state.am_choking) {
                peer->send_unchoke();
            } else if (!should_unchoke && !state.am_choking) {
                peer->send_choke();
            }
        }
    }

} // namespace yuan::net::bit_torrent
