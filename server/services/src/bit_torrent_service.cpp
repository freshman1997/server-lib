#include "bit_torrent_service.h"

#include "eventbus/event_bus.h"
#include "server_service_events.h"

namespace
{

yuan::server::ServiceRuntimeEvent make_event(const yuan::app::RuntimeContext &context)
{
    yuan::server::ServiceRuntimeEvent event;
    event.app_name = context.app_name;
    event.run_mode = context.run_mode;
    event.worker_threads = context.worker_threads;
    event.worker_index = context.worker_index;
    event.is_worker_process = context.is_worker_process;
    event.service_name = "bit_torrent";
    event.protocol = "bittorrent";
    event.port = 0;
    return event;
}

} // namespace

namespace yuan::server
{

BitTorrentService::BitTorrentService(std::string torrent_file_path, std::string save_path)
    : torrent_file_path_(std::move(torrent_file_path))
    , save_path_(std::move(save_path))
    , client_(std::make_unique<yuan::net::bit_torrent::BitTorrentClient>())
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

void BitTorrentService::set_runtime_context(const yuan::app::RuntimeContext &context)
{
    runtime_context_ = context;
}

void BitTorrentService::start()
{
    if (started_.exchange(true) || !client_) {
        return;
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activating, make_event(runtime_context_));
    }

    worker_ = std::thread([this]() {
        client_->start();
        started_.store(false);
    });

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_activated, make_event(runtime_context_));
    }
}

void BitTorrentService::stop()
{
    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopping, make_event(runtime_context_));
    }

    if (client_) {
        client_->stop();
    }

    started_.store(false);
    if (worker_.joinable()) {
        worker_.join();
    }

    if (runtime_context_.event_bus) {
        runtime_context_.event_bus->publish(events::service_stopped, make_event(runtime_context_));
    }
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
