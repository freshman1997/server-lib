#ifndef __YUAN_SERVER_NAS_WEBDAV_MOUNT_H__
#define __YUAN_SERVER_NAS_WEBDAV_MOUNT_H__

#include "nas/nas_metadata_store.h"
#include "nas/nas_share_manager.h"
#include "nas/nas_types.h"

#include <memory>

namespace yuan::net::http
{
    class HttpServer;
}

namespace yuan::server::nas
{
    struct NasWebDavMountResult
    {
        bool mounted = false;
        std::string mount_path;
        std::size_t share_count = 0;
        std::shared_ptr<NasShareManager> share_manager;
    };

    NasWebDavMountResult mount_nas_webdav(
        yuan::net::http::HttpServer &server,
        const NasConfig &config,
        std::shared_ptr<NasMetadataStore> metadata);
}

#endif
