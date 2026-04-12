#ifndef __YUAN_SERVER_SERVICE_EVENTS_H__
#define __YUAN_SERVER_SERVICE_EVENTS_H__

#include "runtime_context.h"

#include <string>

namespace yuan::server::events
{
    inline constexpr const char *service_activating = "server.service.activating";
    inline constexpr const char *service_activated = "server.service.activated";
    inline constexpr const char *service_stopping = "server.service.stopping";
    inline constexpr const char *service_stopped = "server.service.stopped";
}

namespace yuan::server
{

struct ServiceRuntimeEvent
{
    std::string app_name;
    yuan::app::RunMode run_mode = yuan::app::RunMode::single_thread;
    std::size_t worker_threads = 1;
    std::size_t worker_index = 0;
    bool is_worker_process = false;
    std::string service_name;
    std::string protocol;
    int port = 0;
};

} // namespace yuan::server

#endif
