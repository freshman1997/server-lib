#ifndef __SERVER_NAS_SERVICE_H__
#define __SERVER_NAS_SERVICE_H__

#include "http_service.h"
#include "nas/nas.h"
#include "smb.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace yuan::server
{
    struct NasServiceConfig
    {
        struct SmbConfig
        {
            bool enabled = false;
            int port = 445;
            bool require_signing = false;
            bool enable_encryption = false;
            std::string server_name = "YUAN-NAS";
            std::string domain_name = "WORKGROUP";
        };

        int port = 8080;
        yuan::net::http::HttpServerConfig http;
        yuan::server::nas::NasConfig nas;
        SmbConfig smb;
        std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata;
        std::vector<yuan::server::nas::NasUser> bootstrap_users;
    };

    class SmbService;
    class NasSmbHandler;

    class NasService : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit NasService(NasServiceConfig config);
        ~NasService() override;

        bool init() override;
        void start() override;
        void stop() override;
        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool reload(NasServiceConfig config);
        bool reload_from_file(const std::filesystem::path &path);

        HttpService &http_service();
        const HttpService &http_service() const;
        std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata_store() const;
        const NasServiceConfig &config() const;

    private:
        bool prepare_metadata();
        bool apply_bootstrap_data();
        yuan::net::smb::SmbServerConfig build_smb_server_config() const;
        bool init_smb_service();
        void stop_smb_service();
        void refresh_smb_shares();
        std::vector<yuan::server::nas::NasShare> effective_shares() const;
        nlohmann::json health_status_json() const;
        void record_audit(std::string actor,
                          std::string action,
                          std::string target,
                          std::string detail = {});
        std::vector<yuan::server::nas::NasAuditEvent> audit_events(std::size_t limit) const;
        void record_admin_session(const yuan::net::http::HttpRequest *req, const std::string &username);
        void install_health_endpoint();
        void install_admin_endpoints();
        void install_admin_console();

        NasServiceConfig config_;
        std::unique_ptr<HttpService> http_;
        std::unique_ptr<SmbService> smb_;
        std::unique_ptr<NasSmbHandler> smb_handler_;
        yuan::server::nas::NasWebDavMountResult mount_result_;
        yuan::app::RuntimeContext runtime_context_;
        bool has_runtime_context_ = false;
        bool mounted_ = false;
        bool initialized_ = false;
        bool started_ = false;
    };

    std::optional<NasServiceConfig> load_nas_service_config(const std::filesystem::path &path);
}

#endif
