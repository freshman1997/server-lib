#ifndef YUAN_GAME_SERVER_GLOBAL_GLOBAL_PROCESS_SERVICE_H
#define YUAN_GAME_SERVER_GLOBAL_GLOBAL_PROCESS_SERVICE_H

#include "application.h"
#include "global/global_service.h"

#include <cstdint>

namespace yuan::game::server
{
    class GlobalProcessService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        GlobalProcessService(GameServiceId service_id, std::uint16_t port, std::uint16_t tunnel_port);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::uint16_t port_ = 0;
        std::uint16_t tunnel_port_ = 0;
        int listen_fd_ = -1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GlobalService global_;

        bool register_to_tunnel();

        bool call_source_zone(const yuan::rpc::Message &message);
    };
}

#endif
