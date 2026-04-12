#ifndef __MATCH_SERVICE_H__
#define __MATCH_SERVICE_H__

#include "match_server.h"
#include "service.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace match
{

class MatchService : public yuan::app::Service
{
public:
    explicit MatchService(std::string config_path);
    ~MatchService() override;

    bool init() override;
    void start() override;
    void stop() override;

    MatchServer& server();
    const MatchServer& server() const;

private:
    std::string config_path_;
    std::atomic<bool> started_{false};
    std::unique_ptr<MatchServer> server_;
    std::thread worker_;
};

} // namespace match

#endif
