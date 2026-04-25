#ifndef __SERVER_NAS_SMB_ADAPTER_H__
#define __SERVER_NAS_SMB_ADAPTER_H__

#include "nas/nas.h"
#include "smb.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::server
{
    yuan::net::smb::SmbServerConfig make_smb_config_from_nas_shares(
        const std::vector<yuan::server::nas::NasShare> &shares,
        yuan::net::smb::SmbServerConfig base = {});

    yuan::net::smb::SmbServerConfig make_smb_config_from_nas(
        const yuan::server::nas::NasConfig &config,
        const std::shared_ptr<yuan::server::nas::NasMetadataStore> &metadata,
        yuan::net::smb::SmbServerConfig base = {});

    class NasSmbHandler final : public yuan::net::smb::SmbHandler
    {
    public:
        explicit NasSmbHandler(std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata);

        bool on_authenticate(yuan::net::smb::SmbSession *session,
                             const std::string &user,
                             const std::string &domain) override;
        std::optional<std::string> on_password_lookup(yuan::net::smb::SmbSession *session,
                                                      const std::string &user,
                                                      const std::string &domain) override;
        bool on_tree_connect(yuan::net::smb::SmbSession *session, const std::string &path) override;
        void on_logoff(yuan::net::smb::SmbSession *session) override;
        void on_session_closed(yuan::net::smb::SmbSession *session) override;

        bool on_create(yuan::net::smb::SmbSession *session,
                       uint32_t tree_id,
                       const std::string &path,
                       const yuan::net::smb::CreateParams &params) override;
        bool on_read(yuan::net::smb::SmbSession *session,
                     const yuan::net::smb::FileId &file_id,
                     uint64_t offset,
                     uint32_t length) override;
        bool on_write(yuan::net::smb::SmbSession *session,
                      const yuan::net::smb::FileId &file_id,
                      uint64_t offset,
                      uint32_t length) override;
        bool on_query_directory(yuan::net::smb::SmbSession *session,
                                const yuan::net::smb::FileId &file_id) override;
        bool on_query_info(yuan::net::smb::SmbSession *session,
                           const yuan::net::smb::FileId &file_id) override;
        bool on_set_info(yuan::net::smb::SmbSession *session,
                         const yuan::net::smb::FileId &file_id) override;

    private:
        std::optional<yuan::server::nas::NasUser> user_for_session(const yuan::net::smb::SmbSession *session) const;
        std::optional<yuan::server::nas::NasShare> share_for_tree(yuan::net::smb::SmbSession *session, uint32_t tree_id) const;
        std::optional<yuan::server::nas::NasShare> share_for_file(yuan::net::smb::SmbSession *session,
                                                                  const yuan::net::smb::FileId &file_id) const;
        bool allowed(yuan::net::smb::SmbSession *session,
                     const yuan::server::nas::NasShare &share,
                     yuan::server::nas::NasPermission required) const;

        std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata_;
        yuan::server::nas::NasPermissionService permissions_;
        mutable std::mutex session_users_mutex_;
        std::unordered_map<uint64_t, yuan::server::nas::NasUser> session_users_;
    };
}

#endif
