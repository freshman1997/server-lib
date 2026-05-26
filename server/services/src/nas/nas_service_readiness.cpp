#include "nas/nas_service_readiness.h"

#include <filesystem>

namespace yuan::server
{
    namespace
    {
        bool is_legacy_or_plain_hash(std::string_view password_hash)
        {
            return password_hash.rfind("plain:", 0) == 0 ||
                   password_hash.rfind("fnv1a64$", 0) == 0;
        }

        bool is_pbkdf2_hash(std::string_view password_hash)
        {
            return password_hash.rfind("pbkdf2-sha256$", 0) == 0;
        }

        bool share_path_is_relative(const yuan::server::nas::NasShare &share)
        {
            return !share.root_path.empty() && std::filesystem::path(share.root_path).is_relative();
        }

        void push_issue(nlohmann::json &items,
                        std::uint32_t &mask,
                        std::uint32_t bit,
                        const char *code,
                        const char *severity,
                        const char *message,
                        bool blocker)
        {
            mask |= bit;
            nlohmann::json issue;
            issue["code"] = code;
            issue["severity"] = severity;
            issue["blocker"] = blocker;
            issue["message"] = message;
            items.push_back(std::move(issue));
        }
    }

    nlohmann::json build_nas_service_readiness_json(NasServiceReadinessInput input)
    {
        constexpr std::uint32_t kIssueNotInitialized = 1u << 0;
        constexpr std::uint32_t kIssueNotMounted = 1u << 1;
        constexpr std::uint32_t kIssueMetadataUnavailable = 1u << 2;
        constexpr std::uint32_t kIssueNoAdminUser = 1u << 3;
        constexpr std::uint32_t kIssueNoEnabledShare = 1u << 4;
        constexpr std::uint32_t kIssueNoWritableShare = 1u << 5;
        constexpr std::uint32_t kIssueAnonymousReadEnabled = 1u << 6;
        constexpr std::uint32_t kIssueSmbSigningDisabled = 1u << 7;
        constexpr std::uint32_t kIssueWeakPasswordHash = 1u << 8;
        constexpr std::uint32_t kIssueRelativeSharePath = 1u << 9;
        constexpr std::uint32_t kIssueAuditFileDisabled = 1u << 10;

        bool has_admin_user = false;
        bool has_weak_password_hash = false;
        bool has_non_pbkdf2_password_hash = false;
        for (const auto &user : input.users) {
            if (!user.enabled) {
                continue;
            }
            if (user.admin) {
                has_admin_user = true;
            }
            has_weak_password_hash = has_weak_password_hash || is_legacy_or_plain_hash(user.password_hash);
            has_non_pbkdf2_password_hash = has_non_pbkdf2_password_hash || !is_pbkdf2_hash(user.password_hash);
        }

        bool has_enabled_share = false;
        bool has_writable_share = false;
        bool has_relative_share_path = false;
        for (const auto &share : input.shares) {
            if (!share.enabled) {
                continue;
            }
            has_enabled_share = true;
            has_relative_share_path = has_relative_share_path || share_path_is_relative(share);
            if (!share.readonly &&
                yuan::server::nas::has_permission(share.default_permissions,
                                                  yuan::server::nas::NasPermission::write)) {
                has_writable_share = true;
            }
        }

        nlohmann::json blockers = nlohmann::json::array();
        nlohmann::json warnings = nlohmann::json::array();
        std::uint32_t blocker_mask = 0;
        std::uint32_t warning_mask = 0;

        if (!input.initialized) {
            push_issue(blockers, blocker_mask, kIssueNotInitialized,
                       "service_not_initialized", "error",
                       "NAS service is not initialized", true);
        }
        if (!input.mounted) {
            push_issue(blockers, blocker_mask, kIssueNotMounted,
                       "webdav_not_mounted", "error",
                       "NAS WebDAV mount is not active", true);
        }
        if (!input.metadata_available) {
            push_issue(blockers, blocker_mask, kIssueMetadataUnavailable,
                       "metadata_unavailable", "error",
                       "Metadata store is unavailable", true);
        }
        if (!has_admin_user) {
            push_issue(blockers, blocker_mask, kIssueNoAdminUser,
                       "admin_user_missing", "error",
                       "No enabled NAS admin user configured", true);
        }
        if (!has_enabled_share) {
            push_issue(blockers, blocker_mask, kIssueNoEnabledShare,
                       "share_missing", "error",
                       "No enabled NAS share configured", true);
        }
        if (!has_writable_share) {
            push_issue(blockers, blocker_mask, kIssueNoWritableShare,
                       "writable_share_missing", "error",
                       "No writable NAS share available", true);
        }

        const auto anonymous_is_blocker = input.config.production_mode;
        if (input.config.nas.allow_anonymous_read) {
            push_issue(anonymous_is_blocker ? blockers : warnings,
                       anonymous_is_blocker ? blocker_mask : warning_mask,
                       kIssueAnonymousReadEnabled,
                       "anonymous_read_enabled",
                       anonymous_is_blocker ? "error" : "warning",
                       "Anonymous read is enabled",
                       anonymous_is_blocker);
        }

        const auto smb_signing_is_blocker = input.config.production_mode && input.config.smb.enabled;
        if (input.config.smb.enabled && !input.config.smb.require_signing) {
            push_issue(smb_signing_is_blocker ? blockers : warnings,
                       smb_signing_is_blocker ? blocker_mask : warning_mask,
                       kIssueSmbSigningDisabled,
                       "smb_signing_disabled",
                       smb_signing_is_blocker ? "error" : "warning",
                       "SMB signing is disabled",
                       smb_signing_is_blocker);
        }

        if (has_weak_password_hash || (input.config.production_mode && has_non_pbkdf2_password_hash)) {
            push_issue(blockers, blocker_mask, kIssueWeakPasswordHash,
                       "weak_password_hash", "error",
                       "Production NAS users must use pbkdf2-sha256 password hashes",
                       true);
        }
        if (input.config.production_mode && has_relative_share_path) {
            push_issue(blockers, blocker_mask, kIssueRelativeSharePath,
                       "relative_share_path", "error",
                       "Production share root paths must be absolute",
                       true);
        }
        if (input.config.production_mode && !input.config.nas.audit.file_enabled) {
            push_issue(warnings, warning_mask, kIssueAuditFileDisabled,
                       "audit_file_disabled", "warning",
                       "File audit fallback is disabled",
                       false);
        }

        nlohmann::json body;
        body["ready"] = blocker_mask == 0;
        body["production_mode"] = input.config.production_mode;
        body["score"] = blocker_mask == 0 ? 100 : 0;
        body["blocker_count"] = blockers.size();
        body["warning_count"] = warnings.size();
        body["blockers"] = std::move(blockers);
        body["warnings"] = std::move(warnings);
        body["blocker_mask"] = blocker_mask;
        body["warning_mask"] = warning_mask;
        body["checks"] = {
            {"initialized", input.initialized},
            {"started", input.started},
            {"mounted", input.mounted},
            {"metadata_available", input.metadata_available},
            {"admin_user_present", has_admin_user},
            {"enabled_share_present", has_enabled_share},
            {"writable_share_present", has_writable_share},
            {"anonymous_read_enabled", input.config.nas.allow_anonymous_read},
            {"smb_enabled", input.config.smb.enabled},
            {"smb_require_signing", input.config.smb.require_signing},
            {"production_mode", input.config.production_mode},
            {"pbkdf2_password_hashes_only", !has_non_pbkdf2_password_hash},
            {"absolute_share_paths", !has_relative_share_path}
        };
        return body;
    }
}
