#ifndef YUAN_GAME_SERVER_TUNNEL_TUNNEL_PROCESS_SERVICE_H
#define YUAN_GAME_SERVER_TUNNEL_TUNNEL_PROCESS_SERVICE_H

#include "application.h"
#include "tunnel/tunnel_service.h"

#include <cstdint>

namespace yuan::game::server
{
    class TunnelProcessService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        TunnelProcessService(GameServiceId service_id, std::uint16_t listen_port, std::size_t expected_requests);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::uint16_t listen_port_ = 0;
        std::size_t expected_requests_ = 1;
        int listen_fd_ = -1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        TunnelService tunnel_;
    };
}

#endif
