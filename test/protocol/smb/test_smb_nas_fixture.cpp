#include "nas/nas_smb_adapter.h"
#include "smb/smb_service.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    bool parse_bool_env(const char *value)
    {
        if (value == nullptr) {
            return false;
        }

        std::string normalized(value);
        for (char &ch : normalized) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
    }

    class FixtureMetadataStore final : public yuan::server::nas::NasMetadataStore
    {
    public:
        bool available() const override { return true; }

        std::optional<yuan::server::nas::NasUser> find_user_by_name(std::string_view name) const override
        {
            for (const auto &user : users_) {
                if (user.username == name) {
                    return user;
                }
            }
            return std::nullopt;
        }

        std::vector<yuan::server::nas::NasUser> list_users() const override
        {
            return users_;
        }

        bool upsert_user(const yuan::server::nas::NasUser &user) override
        {
            for (auto &item : users_) {
                if (item.id == user.id || item.username == user.username) {
                    item = user;
                    return true;
                }
            }
            users_.push_back(user);
            return true;
        }

        std::optional<yuan::server::nas::NasShare> find_share_by_name(std::string_view name) const override
        {
            for (const auto &share : shares_) {
                if (share.name == name) {
                    return share;
                }
            }
            return std::nullopt;
        }

        std::vector<yuan::server::nas::NasShare> list_shares() const override
        {
            return shares_;
        }

        bool upsert_share(const yuan::server::nas::NasShare &share) override
        {
            for (auto &item : shares_) {
                if (item.id == share.id || item.name == share.name) {
                    item = share;
                    return true;
                }
            }
            shares_.push_back(share);
            return true;
        }

        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override
        {
            return yuan::server::nas::NasPermission::none;
        }

        bool set_permissions(std::string_view, std::string_view, yuan::server::nas::NasPermission) override
        {
            return true;
        }

        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override
        {
            return {};
        }

        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override
        {
            return true;
        }

        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override
        {
            return true;
        }

        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &) override
        {
            return true;
        }

        bool try_create_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override
        {
            return upsert_webdav_lock(lock);
        }

        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view) const override
        {
            return std::nullopt;
        }

        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view, std::string_view) const override
        {
            return {};
        }

        bool remove_webdav_lock(std::string_view) override
        {
            return true;
        }

        std::size_t prune_expired_webdav_locks() override
        {
            return 0;
        }

        bool append_audit_event(const yuan::server::nas::NasAuditEvent &) override
        {
            return true;
        }

        std::vector<yuan::server::nas::NasAuditEvent> list_audit_events(std::size_t) const override
        {
            return {};
        }

        bool upsert_admin_session(const yuan::server::nas::NasAdminSession &) override
        {
            return true;
        }

        std::vector<yuan::server::nas::NasAdminSession> list_admin_sessions(std::size_t) const override
        {
            return {};
        }

    private:
        std::vector<yuan::server::nas::NasUser> users_;
        std::vector<yuan::server::nas::NasShare> shares_;
    };
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        std::cerr << "usage: test_smb_nas_fixture <share_root> <share_name> <username> <password> [port] [ready_file]" << std::endl;
        return 2;
    }

    const std::string share_root = argv[1];
    const std::string share_name = argv[2];
    const std::string username = argv[3];
    const std::string password = argv[4];
    const int port = argc > 5 ? std::atoi(argv[5]) : 15445;
    const std::string ready_file = argc > 6 ? argv[6] : std::string{};

    std::error_code ec;
    std::filesystem::create_directories(share_root, ec);
    if (ec) {
        std::cerr << "failed to create share root: " << ec.message() << std::endl;
        return 2;
    }

    auto metadata = std::make_shared<FixtureMetadataStore>();

    yuan::server::nas::NasUser user;
    user.id = "fixture-user";
    user.username = username;
    user.password_hash = "plain:" + password;
    user.enabled = true;
    metadata->upsert_user(user);

    yuan::server::nas::NasShare share;
    share.id = "fixture-share";
    share.name = share_name;
    share.root_path = share_root;
    share.enabled = true;
    share.default_permissions = yuan::server::nas::NasPermission::none;
    share.subject_permissions[user.id] = yuan::server::nas::NasPermission::read |
                                         yuan::server::nas::NasPermission::write |
                                         yuan::server::nas::NasPermission::remove;
    metadata->upsert_share(share);

    yuan::server::nas::NasConfig nas_cfg;
    nas_cfg.shares.push_back(share);

    auto smb_cfg = yuan::server::make_smb_config_from_nas(nas_cfg, metadata);
    smb_cfg.server_name = "YUAN-NAS";
    smb_cfg.domain_name = "WORKGROUP";
    smb_cfg.require_signing = parse_bool_env(std::getenv("YUAN_SMB_REQUIRE_SIGNING"));

    yuan::server::SmbService service(port, smb_cfg);
    auto handler = std::make_unique<yuan::server::NasSmbHandler>(metadata);
    service.set_handler(handler.get());

    if (!service.init()) {
        std::cerr << "failed to init smb service" << std::endl;
        return 2;
    }

    std::atomic<bool> running{ true };
    std::thread service_thread([&]() { service.start(); });

    std::cout << "READY" << std::endl;
    std::cout.flush();
    if (!ready_file.empty()) {
        std::ofstream out(ready_file, std::ios::binary);
        if (out.good()) {
            out << "READY\n";
        }
    }

    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    service.stop();
    if (service_thread.joinable()) {
        service_thread.join();
    }

    return 0;
}
