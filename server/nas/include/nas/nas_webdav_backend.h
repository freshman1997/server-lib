#ifndef __YUAN_SERVER_NAS_WEBDAV_BACKEND_H__
#define __YUAN_SERVER_NAS_WEBDAV_BACKEND_H__

#include "nas/nas_metadata_store.h"
#include "nas/nas_share_manager.h"
#include "nas/nas_webdav_adapter.h"

#include "webdav_resource.h"

#include <memory>

namespace yuan::server::nas
{
    class NasWebDavBackend final : public yuan::net::webdav::WebDavResourceBackend
    {
    public:
        NasWebDavBackend(std::shared_ptr<NasShareManager> shares,
                         std::shared_ptr<NasMetadataStore> metadata = nullptr,
                         std::string mount_path = "/dav");

        yuan::net::webdav::ResourceInfo stat(std::string_view href) const override;
        std::vector<yuan::net::webdav::ChildResource> list(std::string_view href) const override;
        std::optional<std::string> read(std::string_view href) const override;
        std::optional<std::filesystem::path> file_path_for_read(std::string_view href) const override;
        yuan::net::webdav::BackendResult write(std::string_view href, std::string_view body, bool create_parents) override;
        yuan::net::webdav::BackendResult write_from_file(std::string_view href,
                                                         const std::filesystem::path &source,
                                                         bool create_parents) override;
        yuan::net::webdav::BackendResult make_collection(std::string_view href) override;
        yuan::net::webdav::BackendResult remove(std::string_view href) override;
        yuan::net::webdav::BackendResult copy(std::string_view from, std::string_view to, bool overwrite) override;
        yuan::net::webdav::BackendResult move(std::string_view from, std::string_view to, bool overwrite) override;
        yuan::net::webdav::BackendResult set_properties(std::string_view href, const std::vector<yuan::net::webdav::Property> &properties) override;
        std::vector<yuan::net::webdav::Property> dead_properties(std::string_view href) const override;
        yuan::net::webdav::Quota quota(std::string_view href) const override;

    private:
        struct ResolvedBackend
        {
            NasShare share;
            std::string relative_href;
        };

        std::optional<ResolvedBackend> resolve_backend(std::string_view href) const;
        static std::string to_backend_href(std::string relative_path);

        std::shared_ptr<NasShareManager> shares_;
        std::shared_ptr<NasMetadataStore> metadata_;
        NasWebDavAdapter adapter_;
    };
}

#endif
