#include "match_service.h"

namespace match
{

MatchService::MatchService(std::string config_path)
    : config_path_(std::move(config_path))
    , server_(std::make_unique<MatchServer>())
{
}

MatchService::~MatchService()
{
    stop();
}

bool MatchService::init()
{
    return server_ && server_->init(config_path_);
}

void MatchService::start()
{
    if (started_.exchange(true) || !server_) {
        return;
    }

    worker_ = std::thread([this]() {
        server_->run();
        started_.store(false);
    });
}

bool MatchService::stop()
{
    if (!server_) {
        return true;
    }

    server_->stop();
    started_.store(false);

    if (worker_.joinable()) {
        worker_.join();
    }
    return true;
}

MatchServer& MatchService::server()
{
    return *server_;
}

const MatchServer& MatchService::server() const
{
    return *server_;
}

} // namespace match
