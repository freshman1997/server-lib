#include "nas/nas_webdav_adapter.h"

namespace yuan::server::nas
{
    NasWebDavAdapter::NasWebDavAdapter(std::string mount_path)
        : mount_path_(normalize_mount(std::move(mount_path)))
    {
    }

    std::optional<NasWebDavRoute> NasWebDavAdapter::parse_route(std::string_view request_path) const
    {
        std::string path(request_path);
        if (path.empty() || path.front() != '/') {
            path.insert(path.begin(), '/');
        }
        const auto query = path.find('?');
        if (query != std::string::npos) {
            path.resize(query);
        }

        if (path != mount_path_ && path.rfind(mount_path_ + "/", 0) != 0) {
            return std::nullopt;
        }

        std::string rest = path == mount_path_ ? "" : path.substr(mount_path_.size() + 1);
        const auto slash = rest.find('/');
        NasWebDavRoute route;
        route.share_name = slash == std::string::npos ? rest : rest.substr(0, slash);
        route.relative_path = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (route.share_name.empty()) {
            return std::nullopt;
        }
        route.relative_path = NasShareManager::normalize_relative_path(route.relative_path);
        if (route.relative_path == "..") {
            return std::nullopt;
        }
        return route;
    }

    std::optional<NasResolvedPath> NasWebDavAdapter::resolve(const NasShareManager &shares, std::string_view request_path) const
    {
        auto route = parse_route(request_path);
        if (!route) {
            return std::nullopt;
        }
        return shares.resolve(route->share_name, route->relative_path);
    }

    const std::string &NasWebDavAdapter::mount_path() const
    {
        return mount_path_;
    }

    std::string NasWebDavAdapter::normalize_mount(std::string mount_path)
    {
        if (mount_path.empty() || mount_path.front() != '/') {
            mount_path.insert(mount_path.begin(), '/');
        }
        while (mount_path.size() > 1 && mount_path.back() == '/') {
            mount_path.pop_back();
        }
        return mount_path;
    }
}
