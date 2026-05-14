#include "nas_smb_adapter.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    class FixtureMetadataStore final : public yuan::server::nas::NasMetadataStore
    {
    public:
        bool available() const override { return true; }

        std::optional<yuan::server::nas::NasUser> find_user_by_name(std::string_view name) const override
        {
            auto it = users_by_name.find(std::string(name));
            return it == users_by_name.end() ? std::nullopt : std::optional<yuan::server::nas::NasUser>(it->second);
        }

        std::vector<yuan::server::nas::NasUser> list_users() const override
        {
            std::vector<yuan::server::nas::NasUser> out;
            for (const auto &[name, user] : users_by_name) {
                out.push_back(user);
            }
            return out;
        }

        bool upsert_user(const yuan::server::nas::NasUser &user) override
        {
            users_by_name[user.username] = user;
            return true;
        }

        std::optional<yuan::server::nas::NasShare> find_share_by_name(std::string_view name) const override
        {
            auto it = shares_by_name.find(std::string(name));
            return it == shares_by_name.end() ? std::nullopt : std::optional<yuan::server::nas::NasShare>(it->second);
        }

        std::vector<yuan::server::nas::NasShare> list_shares() const override
        {
            std::vector<yuan::server::nas::NasShare> out;
            for (const auto &[name, share] : shares_by_name) {
                out.push_back(share);
            }
            return out;
        }

        bool upsert_share(const yuan::server::nas::NasShare &share) override
        {
            shares_by_name[share.name] = share;
            return true;
        }

        yuan::server::nas::NasPermission permissions_for(std::string_view, std::string_view) const override
        {
            return yuan::server::nas::NasPermission::none;
        }
        bool set_permissions(std::string_view, std::string_view, yuan::server::nas::NasPermission) override { return true; }
        std::unordered_map<std::string, std::string> dead_properties(std::string_view, std::string_view) const override { return {}; }
        bool set_dead_property(std::string_view, std::string_view, std::string_view, std::string_view) override { return true; }
        bool remove_dead_property(std::string_view, std::string_view, std::string_view) override { return true; }
        bool upsert_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &) override { return true; }
        bool try_create_webdav_lock(const yuan::server::nas::NasWebDavLockRecord &lock) override { return upsert_webdav_lock(lock); }
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view) const override { return std::nullopt; }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view, std::string_view) const override { return {}; }
        bool remove_webdav_lock(std::string_view) override { return true; }
        std::size_t prune_expired_webdav_locks() override { return 0; }
        bool append_audit_event(const yuan::server::nas::NasAuditEvent &) override { return true; }
        std::vector<yuan::server::nas::NasAuditEvent> list_audit_events(std::size_t) const override { return {}; }
        bool upsert_admin_session(const yuan::server::nas::NasAdminSession &) override { return true; }
        std::vector<yuan::server::nas::NasAdminSession> list_admin_sessions(std::size_t) const override { return {}; }

        std::unordered_map<std::string, yuan::server::nas::NasUser> users_by_name;
        std::unordered_map<std::string, yuan::server::nas::NasShare> shares_by_name;
    };

    void check(bool cond, const std::string &msg)
    {
        if (!cond) {
            std::cerr << "FAIL: " << msg << std::endl;
            std::exit(1);
        }
    }
}

int main(int argc, char **argv)
{
    namespace nas = yuan::server::nas;
    namespace smb = yuan::net::smb;

    enum class SmokeMode
    {
        all,
        basic,
        ioctl,
    };

    SmokeMode mode = SmokeMode::all;
    if (argc >= 3 && std::string(argv[1]) == "--mode") {
        const std::string value = argv[2];
        if (value == "basic") {
            mode = SmokeMode::basic;
        } else if (value == "ioctl") {
            mode = SmokeMode::ioctl;
        }
    }

    constexpr uint32_t desired_file_read_data = 0x00000001u;
    constexpr uint32_t desired_file_write_data = 0x00000002u;
    constexpr uint32_t disposition_file_open = 0x00000001u;
    constexpr uint32_t disposition_file_open_if = 0x00000003u;
    constexpr uint32_t fsctl_validate_negotiate_info = 0x00140204u;
    constexpr uint32_t fsctl_set_reparse_point = 0x000900A4u;
    constexpr uint32_t fsctl_unknown = 0x00ABCDEFu;

    const auto root = (std::filesystem::temp_directory_path() / "yuan_smb_internal_client_smoke").string();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    auto metadata = std::make_shared<FixtureMetadataStore>();

    nas::NasUser reader;
    reader.id = "reader-id";
    reader.username = "reader";
    reader.password_hash = "plain:reader-secret";
    reader.enabled = true;
    metadata->upsert_user(reader);

    nas::NasUser writer;
    writer.id = "writer-id";
    writer.username = "writer";
    writer.password_hash = "plain:writer-secret";
    writer.enabled = true;
    metadata->upsert_user(writer);

    nas::NasShare share;
    share.id = "docs-id";
    share.name = "docs";
    share.root_path = root;
    share.default_permissions = nas::NasPermission::read;
    share.subject_permissions[writer.id] = nas::NasPermission::read |
                                         nas::NasPermission::write |
                                         nas::NasPermission::remove;
    metadata->upsert_share(share);

    auto smb_cfg = yuan::server::make_smb_config_from_nas(nas::NasConfig{}, metadata);
    check(smb_cfg.shares.size() == 1 && smb_cfg.shares[0].name == "docs", "NAS share should map into SMB config");

    yuan::server::NasSmbHandler handler(metadata);

    smb::SmbShareConfig smb_share_cfg;
    smb_share_cfg.name = "docs";
    smb_share_cfg.path = root;
    smb::SmbShare smb_share(smb_share_cfg);

    smb::SmbSession writer_session(2, nullptr);
    check(handler.on_authenticate(&writer_session, "writer", "WORKGROUP"), "writer authenticate");

    smb::TreeConnection writer_tree;
    writer_tree.share_name = "docs";
    writer_tree.share = &smb_share;
    const auto writer_tree_id = writer_session.add_tree_connection(std::move(writer_tree));
    check(handler.on_tree_connect(&writer_session, "\\\\localhost\\docs"), "writer tree connect");

    smb::CreateParams writer_open;
    writer_open.desired_access = desired_file_write_data;
    writer_open.create_disposition = disposition_file_open_if;
    check(handler.on_create(&writer_session, writer_tree_id, "file.txt", writer_open), "writer create open-if");

    auto fid = writer_session.allocate_file_id();
    smb::OpenFile open_file;
    open_file.file_id = fid;
    open_file.tree_id = writer_tree_id;
    smb_share.add_open_file(fid, open_file);

    if (mode != SmokeMode::ioctl) {
        check(handler.on_write(&writer_session, fid, 0, 4), "writer write");
        check(handler.on_read(&writer_session, fid, 0, 4), "writer read");
        check(handler.on_rename(&writer_session, fid, "renamed.txt"), "writer rename");
        check(handler.on_delete(&writer_session, fid), "writer delete");
        check(handler.on_lock(&writer_session, fid), "writer lock");
    }

    smb::SmbSession reader_session(1, nullptr);
    check(handler.on_authenticate(&reader_session, "reader", "WORKGROUP"), "reader authenticate");
    smb::TreeConnection reader_tree;
    reader_tree.share_name = "docs";
    reader_tree.share = &smb_share;
    const auto reader_tree_id = reader_session.add_tree_connection(std::move(reader_tree));
    (void)reader_tree_id;
    check(handler.on_tree_connect(&reader_session, "\\\\localhost\\docs"), "reader tree connect");

    smb::CreateParams reader_open;
    reader_open.desired_access = desired_file_read_data;
    reader_open.create_disposition = disposition_file_open;
    check(handler.on_create(&reader_session, reader_tree_id, "file.txt", reader_open), "reader open for read");
    if (mode != SmokeMode::ioctl) {
        check(!handler.on_write(&reader_session, fid, 0, 4), "reader write denied");
        check(!handler.on_rename(&reader_session, fid, "x.txt"), "reader rename denied");
        check(!handler.on_delete(&reader_session, fid), "reader delete denied");
    }

    if (mode != SmokeMode::basic) {
        check(handler.on_ioctl(&writer_session, fid, fsctl_validate_negotiate_info),
              "writer read-like ioctl allowed");
        check(handler.on_ioctl(&writer_session, fid, fsctl_set_reparse_point),
              "writer write-like ioctl allowed");
        check(!handler.on_ioctl(&writer_session, fid, fsctl_unknown),
              "writer unknown ioctl denied by default");

        check(handler.on_ioctl(&reader_session, fid, fsctl_validate_negotiate_info),
              "reader read-like ioctl allowed");
        check(!handler.on_ioctl(&reader_session, fid, fsctl_set_reparse_point),
              "reader write-like ioctl denied");

        handler.set_ioctl_permission(fsctl_validate_negotiate_info, nas::NasPermission::write);
        check(!handler.on_ioctl(&reader_session, fid, fsctl_validate_negotiate_info),
              "reader denied after ioctl policy override to write");
        check(handler.on_ioctl(&writer_session, fid, fsctl_validate_negotiate_info),
              "writer passes after ioctl policy override to write");

        handler.set_ioctl_permission(fsctl_unknown, nas::NasPermission::read);
        check(handler.on_ioctl(&reader_session, fid, fsctl_unknown),
              "reader passes after unknown ioctl mapped to read");

        handler.set_ioctl_permission(fsctl_set_reparse_point, std::nullopt);
        check(!handler.on_ioctl(&writer_session, fid, fsctl_set_reparse_point),
              "writer denied when write-like ioctl explicitly disabled");

        handler.reset_ioctl_permissions();
        check(handler.on_ioctl(&reader_session, fid, fsctl_validate_negotiate_info),
              "reader regains default read-like ioctl after reset");
        check(!handler.on_ioctl(&reader_session, fid, fsctl_unknown),
              "reader loses unknown ioctl override after reset");
    }

    handler.on_logoff(&writer_session);
    check(!handler.on_write(&writer_session, fid, 0, 1), "writer loses permission after logoff");

    std::filesystem::remove_all(root);
    if (mode == SmokeMode::basic) {
        std::cout << "SMB internal client basic smoke passed" << std::endl;
    } else if (mode == SmokeMode::ioctl) {
        std::cout << "SMB internal client ioctl smoke passed" << std::endl;
    } else {
        std::cout << "SMB internal client smoke passed" << std::endl;
    }
    return 0;
}
