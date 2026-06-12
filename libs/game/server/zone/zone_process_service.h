#ifndef YUAN_GAME_SERVER_ZONE_ZONE_PROCESS_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_PROCESS_SERVICE_H

#include "application.h"
#include "tunnel/tunnel_service.h"
#include "zone/zone_service.h"

#include <cstdint>

namespace yuan::game::server
{
    class ZoneProcessService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        ZoneProcessService(GameServiceId service_id, GameServiceId global_service_id, std::uint16_t tunnel_port, std::uint16_t listen_port);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::uint16_t tunnel_port_ = 0;
        std::uint16_t listen_port_ = 0;
        int listen_fd_ = -1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GameServiceId global_service_id_;
        TunnelCluster tunnels_;
        ZoneService zone_;

        bool register_to_tunnel();
    };
}

#endif
