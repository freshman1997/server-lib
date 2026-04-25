#include "nas/nas_webdav_mount.h"

#include "http_server.h"
#include "middleware.h"
#include "nas/nas_auth_service.h"
#include "nas/nas_http_middleware.h"
#include "nas/nas_redis_webdav_lock_manager.h"
#include "nas/nas_share_manager.h"
#include "nas/nas_webdav_backend.h"
#include "webdav_handler.h"

namespace yuan::server::nas
{
    NasWebDavMountResult mount_nas_webdav(
        yuan::net::http::HttpServer &server,
        const NasConfig &config,
        std::shared_ptr<NasMetadataStore> metadata)
    {
        NasWebDavMountResult result;
        result.mount_path = config.webdav_mount.empty() ? "/dav" : config.webdav_mount;

        std::vector<NasShare> shares = config.shares;
        if (metadata && metadata->available()) {
            auto stored_shares = metadata->list_shares();
            if (!stored_shares.empty()) {
                shares = std::move(stored_shares);
            }
        }

        auto share_manager = std::make_shared<NasShareManager>(shares);
        result.share_manager = share_manager;
        auto backend = std::make_shared<NasWebDavBackend>(share_manager, metadata, result.mount_path);

        yuan::net::webdav::WebDavHandlerConfig dav_config;
        dav_config.mount_path = result.mount_path;
        auto locks = std::make_shared<NasRedisWebDavLockManager>(share_manager, metadata, result.mount_path);
        auto handler = std::make_shared<yuan::net::webdav::WebDavHandler>(backend, locks, dav_config);

        auto pipeline = std::make_shared<yuan::net::http::MiddlewarePipeline>();
        NasHttpAuthOptions auth_options;
        auth_options.allow_anonymous_read = config.allow_anonymous_read;
        auth_options.realm = "Yuan NAS";
        pipeline->add(nas_basic_auth_middleware(std::make_shared<NasAuthService>(metadata), auth_options));

        server.on(result.mount_path,
                  [handler](yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp) {
                      handler->handle(req, resp);
                  },
                  pipeline,
                  true);

        result.mounted = true;
        result.share_count = shares.size();
        return result;
    }
}
