#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_SERVICE_H

#include "common/game_messages.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace yuan::game::server
{
    class GlobalService : public ServiceNode
    {
    public:
        explicit GlobalService(ServiceAddress address);

        [[nodiscard]] std::uint64_t request_count() const;

        void set_after_echo(std::function<void(const yuan::rpc::Message &)> callback);
        void register_gm_command(std::string command, std::function<GmCommandResponse(const std::vector<std::string> &)> executor);

    private:
        std::uint64_t request_count_ = 0;
        std::function<void(const yuan::rpc::Message &)> after_echo_;
        std::unordered_map<std::string, std::function<GmCommandResponse(const std::vector<std::string> &)>> gm_executors_;
    };
}

#endif
