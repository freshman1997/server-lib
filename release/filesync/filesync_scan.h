#pragma once
#include "filesync_model.h"
namespace filesync::scan {
model::Manifest paths(const model::Config& config);
std::filesystem::path local_path(const model::Config& config, const std::string& remote);
bool included(const model::Config& config, const std::string& remote, bool directory);
}
