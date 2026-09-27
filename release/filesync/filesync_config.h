#pragma once

#include "filesync_model.h"

namespace filesync::config 
{
    model::Config load(const std::filesystem::path& path);

    std::filesystem::path state_file(const model::Config& config);

    model::Manifest load_state(const model::Config& config);

    void save_state(const model::Config& config, const model::Manifest& manifest);
}
