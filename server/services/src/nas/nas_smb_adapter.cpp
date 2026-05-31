#include "nas/nas_smb_adapter.h"

#include "nas/nas_auth_service.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string_view>

namespace yuan::server
{
    namespace
    {
        std::string share_name_from_unc(const std::string &path)
        {
            auto last_sep = path.find_last_of("\\/");
            return last_sep == std::string::npos ? path : path.substr(last_sep + 1);
        }

        std::optional<std::string> plain_password_from_hash(const std::string &password_hash)
        {
            constexpr std::string_view prefix = "plain:";
            if (password_hash.rfind(prefix, 0) != 0) {
                return std::nullopt;
            }
            return password_hash.substr(prefix.size());
        }

        bool is_pbkdf2_hash(const std::string &password_hash)
        {
            constexpr std::string_view prefix = "pbkdf2-sha256$";
            return std::string_view(password_hash).rfind(prefix, 0) == 0;
        }

        std::optional<std::string> nt_hash_from_hash(const std::string &password_hash)
        {
            constexpr std::string_view nthash_prefix = "nthash:";
            constexpr std::string_view ntlm_prefix = "ntlm:";

            std::string_view value(password_hash);
            if (value.rfind(nthash_prefix, 0) == 0) {
                value.remove_prefix(nthash_prefix.size());
            } else if (value.rfind(ntlm_prefix, 0) == 0) {
                value.remove_prefix(ntlm_prefix.size());
            } else {
                return std::nullopt;
            }

            if (value.size() != 32) {
                return std::nullopt;
            }
            for (char ch : value) {
                if (!std::isxdigit(static_cast<unsigned char>(ch))) {
                    return std::nullopt;
                }
            }
            return std::string("nthash:") + std::string(value);
        }

        std::optional<yuan::server::nas::NasPermission> permission_for_ioctl(uint32_t ctl_code)
        {
            using yuan::server::nas::NasPermission;

            struct IoctlPermissionRule
            {
                uint32_t ctl_code;
                NasPermission required;
            };

            static constexpr IoctlPermissionRule rules[] = {
                { 0x00140204u, NasPermission::read },
                { 0x001401FCu, NasPermission::read },
                { 0x001401D4u, NasPermission::read },
                { 0x00060194u, NasPermission::read },
                { 0x000601B4u, NasPermission::read },
                { 0x000900A8u, NasPermission::read },
                { 0x00144064u, NasPermission::read },
                { 0x00140078u, NasPermission::read },
                { 0x000900A4u, NasPermission::write },
                { 0x000900ACu, NasPermission::write },
                { 0x001440F2u, NasPermission::write },
                { 0x001480F2u, NasPermission::write },
                { 0x0011C017u, NasPermission::write },
            };

            for (const auto &rule : rules) {
                if (rule.ctl_code == ctl_code) {
                    return rule.required;
                }
            }

            return std::nullopt;
        }

        yuan::server::nas::NasPermission permission_for_create(const yuan::net::smb::CreateParams &params)
        {
            using yuan::server::nas::NasPermission;

            constexpr uint32_t desired_generic_write = 0x40000000u;
            constexpr uint32_t desired_file_write_data = 0x00000002u;
            constexpr uint32_t desired_file_append_data = 0x00000004u;
            constexpr uint32_t desired_file_write_attributes = 0x00000100u;
            constexpr uint32_t desired_delete = 0x00010000u;
            constexpr uint32_t create_file_create = 0x00000002u;
            constexpr uint32_t create_file_open_if = 0x00000003u;
            constexpr uint32_t create_file_overwrite = 0x00000004u;
            constexpr uint32_t create_file_overwrite_if = 0x00000005u;
            constexpr uint32_t option_file_delete_on_close = 0x00001000u;

            NasPermission required = NasPermission::read;
            if ((params.desired_access & (desired_generic_write | desired_file_write_data |
                                          desired_file_append_data | desired_file_write_attributes)) != 0 ||
                params.create_disposition == create_file_create ||
                params.create_disposition == create_file_open_if ||
                params.create_disposition == create_file_overwrite ||
                params.create_disposition == create_file_overwrite_if) {
                required = required | NasPermission::write;
            }
            if ((params.desired_access & desired_delete) != 0 ||
                (params.create_options & option_file_delete_on_close) != 0) {
                required = required | NasPermission::remove;
            }
            return required;
        }

    }

    yuan::net::smb::SmbServerConfig make_smb_config_from_nas_shares(
        const std::vector<yuan::server::nas::NasShare> &shares,
        yuan::net::smb::SmbServerConfig base)
    {
        base.shares.clear();
        for (const auto &share : shares) {
            if (!share.enabled || share.name.empty() || share.root_path.empty()) {
                continue;
            }
            yuan::net::smb::SmbShareConfig smb_share;
            smb_share.name = share.name;
            smb_share.comment = "Yuan NAS share";
            smb_share.type = yuan::net::smb::ShareType::DISK;
            smb_share.path = share.root_path;
            smb_share.share_flags = yuan::net::smb::SMB2_SHAREFLAG_ALLOW_NAMESPACE_CACHING;
            base.shares.push_back(std::move(smb_share));
        }
        return base;
    }

    yuan::net::smb::SmbServerConfig make_smb_config_from_nas(
        const yuan::server::nas::NasConfig &config,
        const std::shared_ptr<yuan::server::nas::NasMetadataStore> &metadata,
        yuan::net::smb::SmbServerConfig base)
    {
        auto shares = config.shares;
        if (metadata && metadata->available()) {
            for (const auto &share : metadata->list_shares()) {
                auto it = std::find_if(shares.begin(), shares.end(), [&](const auto &item) {
                    return item.id == share.id || item.name == share.name;
                });
                if (it == shares.end()) {
                    shares.push_back(share);
                } else {
                    *it = share;
                }
            }
        }
        return make_smb_config_from_nas_shares(shares, std::move(base));
    }

    NasSmbHandler::NasSmbHandler(std::shared_ptr<yuan::server::nas::NasMetadataStore> metadata)
        : metadata_(std::move(metadata))
    {
        reset_ioctl_permissions();
    }

    bool NasSmbHandler::on_authenticate(yuan::net::smb::SmbSession *session,
                                        const std::string &user,
                                        const std::string &domain)
    {
        (void)domain;
        if (!metadata_ || !metadata_->available()) {
            return false;
        }
        auto nas_user = metadata_->find_user_by_name(user);
        if (!nas_user || !nas_user->enabled) {
            return false;
        }
        if (session) {
            std::lock_guard<std::mutex> lock(session_users_mutex_);
            session_users_[session->session_id()] = *nas_user;
        }
        return true;
    }

    std::optional<std::string> NasSmbHandler::on_password_lookup(yuan::net::smb::SmbSession *session,
                                                                 const std::string &user,
                                                                 const std::string &domain)
    {
        (void)session;
        (void)domain;
        if (!metadata_ || !metadata_->available()) {
            return std::nullopt;
        }
        auto nas_user = metadata_->find_user_by_name(user);
        if (!nas_user || !nas_user->enabled) {
            return std::nullopt;
        }
        const auto &smb_hash = nas_user->smb_password_hash.empty()
            ? nas_user->password_hash
            : nas_user->smb_password_hash;
        if (is_pbkdf2_hash(smb_hash)) {
            return std::nullopt;
        }
        return plain_password_from_hash(smb_hash);
    }

    std::optional<std::string> NasSmbHandler::on_nt_hash_lookup(yuan::net::smb::SmbSession *session,
                                                                const std::string &user,
                                                                const std::string &domain)
    {
        (void)session;
        (void)domain;
        if (!metadata_ || !metadata_->available()) {
            return std::nullopt;
        }
        auto nas_user = metadata_->find_user_by_name(user);
        if (!nas_user || !nas_user->enabled) {
            return std::nullopt;
        }
        const auto &smb_hash = nas_user->smb_password_hash.empty()
            ? nas_user->password_hash
            : nas_user->smb_password_hash;
        return nt_hash_from_hash(smb_hash);
    }

    bool NasSmbHandler::on_tree_connect(yuan::net::smb::SmbSession *session, const std::string &path)
    {
        if (!metadata_ || !metadata_->available()) {
            return false;
        }
        auto share = metadata_->find_share_by_name(share_name_from_unc(path));
        if (!share) {
            return false;
        }
        return allowed(session, *share, yuan::server::nas::NasPermission::read);
    }

    void NasSmbHandler::on_logoff(yuan::net::smb::SmbSession *session)
    {
        if (session) {
            std::lock_guard<std::mutex> lock(session_users_mutex_);
            session_users_.erase(session->session_id());
        }
    }

    void NasSmbHandler::on_session_closed(yuan::net::smb::SmbSession *session)
    {
        on_logoff(session);
    }

    bool NasSmbHandler::on_create(yuan::net::smb::SmbSession *session,
                                  uint32_t tree_id,
                                  const std::string &path,
                                  const yuan::net::smb::CreateParams &params)
    {
        (void)path;
        auto share = share_for_tree(session, tree_id);
        return share && allowed(session, *share, permission_for_create(params));
    }

    bool NasSmbHandler::on_read(yuan::net::smb::SmbSession *session,
                                const yuan::net::smb::FileId &file_id,
                                uint64_t offset,
                                uint32_t length)
    {
        (void)offset;
        (void)length;
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::read);
    }

    bool NasSmbHandler::on_write(yuan::net::smb::SmbSession *session,
                                 const yuan::net::smb::FileId &file_id,
                                 uint64_t offset,
                                 uint32_t length)
    {
        (void)offset;
        (void)length;
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::write);
    }

    bool NasSmbHandler::on_query_directory(yuan::net::smb::SmbSession *session,
                                           const yuan::net::smb::FileId &file_id)
    {
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::read);
    }

    bool NasSmbHandler::on_query_info(yuan::net::smb::SmbSession *session,
                                      const yuan::net::smb::FileId &file_id)
    {
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::read);
    }

    bool NasSmbHandler::on_set_info(yuan::net::smb::SmbSession *session,
                                    const yuan::net::smb::FileId &file_id)
    {
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::write);
    }

    bool NasSmbHandler::on_rename(yuan::net::smb::SmbSession *session,
                                  const yuan::net::smb::FileId &file_id,
                                  const std::string &new_path)
    {
        (void)new_path;
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::write);
    }

    bool NasSmbHandler::on_delete(yuan::net::smb::SmbSession *session,
                                  const yuan::net::smb::FileId &file_id)
    {
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::remove);
    }

    bool NasSmbHandler::on_lock(yuan::net::smb::SmbSession *session,
                                const yuan::net::smb::FileId &file_id)
    {
        auto share = share_for_file(session, file_id);
        return share && allowed(session, *share, yuan::server::nas::NasPermission::write);
    }

    bool NasSmbHandler::on_ioctl(yuan::net::smb::SmbSession *session,
                                 const yuan::net::smb::FileId &file_id,
                                 uint32_t ctl_code)
    {
        auto share = share_for_file(session, file_id);
        if (!share) {
            return false;
        }
        std::optional<yuan::server::nas::NasPermission> required;
        bool has_override = false;
        {
            std::lock_guard<std::mutex> lock(ioctl_permissions_mutex_);
            auto it = ioctl_permissions_.find(ctl_code);
            if (it != ioctl_permissions_.end()) {
                has_override = true;
                required = it->second;
            }
        }
        if (!has_override) {
            required = permission_for_ioctl(ctl_code);
        }
        if (!required.has_value()) {
            return false;
        }
        return allowed(session, *share, *required);
    }

    void NasSmbHandler::set_ioctl_permission(uint32_t ctl_code,
                                             std::optional<yuan::server::nas::NasPermission> required)
    {
        std::lock_guard<std::mutex> lock(ioctl_permissions_mutex_);
        ioctl_permissions_[ctl_code] = std::move(required);
    }

    void NasSmbHandler::reset_ioctl_permissions()
    {
        std::lock_guard<std::mutex> lock(ioctl_permissions_mutex_);
        ioctl_permissions_.clear();
        ioctl_permissions_[0x00140204u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x001401FCu] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x001401D4u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x00060194u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x000601B4u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x000900A8u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x00144064u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x00140078u] = yuan::server::nas::NasPermission::read;
        ioctl_permissions_[0x000900A4u] = yuan::server::nas::NasPermission::write;
        ioctl_permissions_[0x000900ACu] = yuan::server::nas::NasPermission::write;
        ioctl_permissions_[0x001440F2u] = yuan::server::nas::NasPermission::write;
        ioctl_permissions_[0x001480F2u] = yuan::server::nas::NasPermission::write;
        ioctl_permissions_[0x0011C017u] = yuan::server::nas::NasPermission::write;
    }

    std::optional<yuan::server::nas::NasUser> NasSmbHandler::user_for_session(const yuan::net::smb::SmbSession *session) const
    {
        if (!session) {
            return std::nullopt;
        }
        {
            std::lock_guard<std::mutex> lock(session_users_mutex_);
            auto it = session_users_.find(session->session_id());
            if (it != session_users_.end()) {
                return it->second;
            }
        }
        if (!session->user_name().empty() && metadata_ && metadata_->available()) {
            return metadata_->find_user_by_name(session->user_name());
        }
        return std::nullopt;
    }

    std::optional<yuan::server::nas::NasShare> NasSmbHandler::share_for_tree(yuan::net::smb::SmbSession *session, uint32_t tree_id) const
    {
        if (!session || !metadata_ || !metadata_->available()) {
            return std::nullopt;
        }
        auto *tree = session->find_tree(tree_id);
        if (!tree) {
            return std::nullopt;
        }
        return metadata_->find_share_by_name(tree->share_name);
    }

    std::optional<yuan::server::nas::NasShare> NasSmbHandler::share_for_file(yuan::net::smb::SmbSession *session,
                                                                             const yuan::net::smb::FileId &file_id) const
    {
        if (!session) {
            return std::nullopt;
        }
        for (auto tree_id : session->all_tree_ids()) {
            auto *tree = session->find_tree(tree_id);
            if (!tree || !tree->share) {
                continue;
            }
            if (tree->share->find_open_file(file_id)) {
                return metadata_ && metadata_->available() ? metadata_->find_share_by_name(tree->share_name) : std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool NasSmbHandler::allowed(yuan::net::smb::SmbSession *session,
                                const yuan::server::nas::NasShare &share,
                                yuan::server::nas::NasPermission required) const
    {
        auto user = user_for_session(session);
        if (!user) {
            return false;
        }
        return permissions_.allowed(share, *user, required);
    }

}
