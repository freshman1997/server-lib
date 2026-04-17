#include "bit_torrent_service.h"

namespace yuan::server
{

    BitTorrentService::BitTorrentService(std::string torrent_file_path, std::string save_path)
        : torrent_file_path_(std::move(torrent_file_path)), save_path_(std::move(save_path)), client_(std::make_unique<yuan::net::bit_torrent::BitTorrentClient>()), host_({ "bit_torrent", "bittorrent", 0 })
    {
    }

    BitTorrentService::~BitTorrentService()
    {
        stop();
    }

    bool BitTorrentService::init()
    {
        if (!client_ || torrent_file_path_.empty()) {
            return false;
        }

        if (!client_->load_torrent(torrent_file_path_)) {
            return false;
        }

        client_->set_save_path(save_path_);
        return true;
    }

    void BitTorrentService::set_runtime_context(const yuan::app::RuntimeContext & context)
    {
        host_.set_runtime_context(context);
        shared_runtime_ = context.shared_runtime;
    }

    void BitTorrentService::start()
    {
        if (shared_runtime_) {
            client_->set_runtime(*shared_runtime_);
        }
        host_.start([this]() { client_->start(); });
    }

    void BitTorrentService::stop()
    {
        host_.stop([this]() { client_->stop(); });
    }

    yuan::net::bit_torrent::BitTorrentClient &BitTorrentService::client()
    {
        return *client_;
    }

    const yuan::net::bit_torrent::BitTorrentClient &BitTorrentService::client() const
    {
        return *client_;
    }

} // namespace yuan::server
