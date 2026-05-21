#ifndef __SERVER_NAS_SERVICE_H__
#define __SERVER_NAS_SERVICE_H__

#include "http/http_service.h"
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
        std::optional<std::string> guard_admin_request(yuan::net::http::HttpRequest *req,
                                                       yuan::net::http::HttpResponse *resp);
        void handle_admin_shares(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_shares_get(yuan::net::http::HttpResponse *resp) const;
        void handle_admin_shares_post(yuan::net::http::HttpRequest *req,
                                      yuan::net::http::HttpResponse *resp,
                                      const std::string &actor);
        void handle_admin_users(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_users_get(yuan::net::http::HttpResponse *resp) const;
        void handle_admin_users_post(yuan::net::http::HttpRequest *req,
                                     yuan::net::http::HttpResponse *resp,
                                     const std::string &actor);
        void handle_admin_quota(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_health_actions(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_health_actions_get(yuan::net::http::HttpResponse *resp) const;
        void handle_admin_health_actions_post(yuan::net::http::HttpRequest *req,
                                              yuan::net::http::HttpResponse *resp,
                                              const std::string &actor);
        void handle_admin_audit(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_audit_get(yuan::net::http::HttpRequest *req,
                                    yuan::net::http::HttpResponse *resp) const;
        void handle_admin_sessions(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_sessions_get(yuan::net::http::HttpRequest *req,
                                       yuan::net::http::HttpResponse *resp) const;
        void handle_admin_activity(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_activity_get(yuan::net::http::HttpResponse *resp) const;
        void handle_admin_readiness(yuan::net::http::HttpRequest *req, yuan::net::http::HttpResponse *resp);
        void handle_admin_readiness_get(yuan::net::http::HttpResponse *resp) const;
        nlohmann::json build_admin_shares_json() const;
        nlohmann::json build_admin_users_json() const;
        nlohmann::json build_admin_quota_json() const;
        nlohmann::json build_admin_audit_json(yuan::net::http::HttpRequest *req) const;
        nlohmann::json build_admin_sessions_json(yuan::net::http::HttpRequest *req) const;
        nlohmann::json build_admin_activity_json() const;
        nlohmann::json build_admin_readiness_json() const;
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
