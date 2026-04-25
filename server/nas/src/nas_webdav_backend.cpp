#include "nas/nas_webdav_backend.h"

#include "webdav_resource.h"

namespace yuan::server::nas
{
    namespace
    {
        yuan::net::webdav::BackendResult not_found()
        {
            return yuan::net::webdav::BackendResult::failure(404);
        }

        yuan::net::webdav::LocalWebDavBackend local_backend_for(const NasShare &share)
        {
            return yuan::net::webdav::LocalWebDavBackend(share.root_path);
        }
    }

    NasWebDavBackend::NasWebDavBackend(std::shared_ptr<NasShareManager> shares,
                                       std::shared_ptr<NasMetadataStore> metadata,
                                       std::string mount_path)
        : shares_(std::move(shares)),
          metadata_(std::move(metadata)),
          adapter_(std::move(mount_path))
    {
    }

    yuan::net::webdav::ResourceInfo NasWebDavBackend::stat(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return {};
        }
        auto backend = local_backend_for(resolved->share);
        return backend.stat(resolved->relative_href);
    }

    std::vector<yuan::net::webdav::ChildResource> NasWebDavBackend::list(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return {};
        }
        auto backend = local_backend_for(resolved->share);
        return backend.list(resolved->relative_href);
    }

    std::optional<std::string> NasWebDavBackend::read(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return std::nullopt;
        }
        auto backend = local_backend_for(resolved->share);
        return backend.read(resolved->relative_href);
    }

    std::optional<std::filesystem::path> NasWebDavBackend::file_path_for_read(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return std::nullopt;
        }
        auto backend = local_backend_for(resolved->share);
        return backend.file_path_for_read(resolved->relative_href);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::write(std::string_view href, std::string_view body, bool create_parents)
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return not_found();
        }
        auto backend = local_backend_for(resolved->share);
        return backend.write(resolved->relative_href, body, create_parents);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::write_from_file(std::string_view href,
                                                                       const std::filesystem::path &source,
                                                                       bool create_parents)
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return not_found();
        }
        auto backend = local_backend_for(resolved->share);
        return backend.write_from_file(resolved->relative_href, source, create_parents);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::make_collection(std::string_view href)
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return not_found();
        }
        auto backend = local_backend_for(resolved->share);
        return backend.make_collection(resolved->relative_href);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::remove(std::string_view href)
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return not_found();
        }
        auto backend = local_backend_for(resolved->share);
        return backend.remove(resolved->relative_href);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::copy(std::string_view from, std::string_view to, bool overwrite)
    {
        auto src = resolve_backend(from);
        auto dst = resolve_backend(to);
        if (!src || !dst) {
            return not_found();
        }
        if (src->share.id != dst->share.id) {
            return yuan::net::webdav::BackendResult::failure(409, "cross-share copy is not supported yet");
        }
        auto backend = local_backend_for(src->share);
        return backend.copy(src->relative_href, dst->relative_href, overwrite);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::move(std::string_view from, std::string_view to, bool overwrite)
    {
        auto src = resolve_backend(from);
        auto dst = resolve_backend(to);
        if (!src || !dst) {
            return not_found();
        }
        if (src->share.id != dst->share.id) {
            return yuan::net::webdav::BackendResult::failure(409, "cross-share move is not supported yet");
        }
        auto backend = local_backend_for(src->share);
        return backend.move(src->relative_href, dst->relative_href, overwrite);
    }

    yuan::net::webdav::BackendResult NasWebDavBackend::set_properties(
        std::string_view href,
        const std::vector<yuan::net::webdav::Property> &properties)
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return not_found();
        }
        if (metadata_ && metadata_->available()) {
            for (const auto &property : properties) {
                if (!metadata_->set_dead_property(resolved->share.id, resolved->relative_href, property.name, property.value)) {
                    return yuan::net::webdav::BackendResult::failure(507);
                }
            }
            return yuan::net::webdav::BackendResult::success(207);
        }
        auto backend = local_backend_for(resolved->share);
        return backend.set_properties(resolved->relative_href, properties);
    }

    std::vector<yuan::net::webdav::Property> NasWebDavBackend::dead_properties(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return {};
        }
        if (metadata_ && metadata_->available()) {
            std::vector<yuan::net::webdav::Property> props;
            for (const auto &[name, value] : metadata_->dead_properties(resolved->share.id, resolved->relative_href)) {
                props.push_back(yuan::net::webdav::Property{ "DAV:", name, value });
            }
            return props;
        }
        auto backend = local_backend_for(resolved->share);
        return backend.dead_properties(resolved->relative_href);
    }

    yuan::net::webdav::Quota NasWebDavBackend::quota(std::string_view href) const
    {
        auto resolved = resolve_backend(href);
        if (!resolved) {
            return {};
        }
        auto backend = local_backend_for(resolved->share);
        return backend.quota(resolved->relative_href);
    }

    std::optional<NasWebDavBackend::ResolvedBackend> NasWebDavBackend::resolve_backend(std::string_view href) const
    {
        if (!shares_) {
            return std::nullopt;
        }
        auto route = adapter_.parse_route(std::string(adapter_.mount_path()) + std::string(href));
        if (!route) {
            return std::nullopt;
        }
        auto share = shares_->find_by_name(route->share_name);
        if (!share) {
            return std::nullopt;
        }
        ResolvedBackend out;
        out.share = *share;
        out.relative_href = to_backend_href(route->relative_path);
        return out;
    }

    std::string NasWebDavBackend::to_backend_href(std::string relative_path)
    {
        if (relative_path.empty()) {
            return "/";
        }
        if (relative_path.front() != '/') {
            relative_path.insert(relative_path.begin(), '/');
        }
        return relative_path;
    }
}
