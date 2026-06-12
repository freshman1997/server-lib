#ifndef YUAN_GAME_SERVER_COMMON_APPLICATION_RUNTIME_H
#define YUAN_GAME_SERVER_COMMON_APPLICATION_RUNTIME_H

#include "application.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    inline yuan::app::RuntimeContext make_game_runtime_context(std::string app_name)
    {
        yuan::app::RuntimeContext context;
        context.app_name = std::move(app_name);
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        return context;
    }
}

#endif
