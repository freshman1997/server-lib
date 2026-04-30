#include "nas_smb_adapter.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace
{
    void check(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << std::endl;
            std::exit(1);
        }
    }

    class MemoryMetadataStore final : public yuan::server::nas::NasMetadataStore
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
        std::optional<yuan::server::nas::NasWebDavLockRecord> find_webdav_lock(std::string_view) const override { return std::nullopt; }
        std::vector<yuan::server::nas::NasWebDavLockRecord> list_webdav_locks(std::string_view, std::string_view) const override { return {}; }
        bool remove_webdav_lock(std::string_view) override { return true; }
        bool append_audit_event(const yuan::server::nas::NasAuditEvent &) override { return true; }
        std::vector<yuan::server::nas::NasAuditEvent> list_audit_events(std::size_t) const override { return {}; }
        bool upsert_admin_session(const yuan::server::nas::NasAdminSession &) override { return true; }
        std::vector<yuan::server::nas::NasAdminSession> list_admin_sessions(std::size_t) const override { return {}; }

        std::unordered_map<std::string, yuan::server::nas::NasUser> users_by_name;
        std::unordered_map<std::string, yuan::server::nas::NasShare> shares_by_name;
    };

    yuan::server::nas::NasShare make_share(const std::string &root)
    {
        yuan::server::nas::NasShare share;
        share.id = "docs-id";
        share.name = "docs";
        share.root_path = root;
        share.default_permissions = yuan::server::nas::NasPermission::read;
        share.subject_permissions["writer-id"] = yuan::server::nas::NasPermission::read |
                                                 yuan::server::nas::NasPermission::write |
                                                 yuan::server::nas::NasPermission::remove;
        return share;
    }
}

int main()
{
    namespace nas = yuan::server::nas;
    namespace smb = yuan::net::smb;

    constexpr uint32_t desired_file_read_data = 0x00000001u;
    constexpr uint32_t desired_file_write_data = 0x00000002u;
    constexpr uint32_t disposition_file_open = 0x00000001u;
    constexpr uint32_t disposition_file_open_if = 0x00000003u;
    constexpr uint32_t fsctl_validate_negotiate_info = 0x00140204u;
    constexpr uint32_t fsctl_set_reparse_point = 0x000900A4u;
    constexpr uint32_t fsctl_unknown = 0x00ABCDEFu;

    const auto root = (std::filesystem::temp_directory_path() / "yuan_nas_smb_adapter_test").string();
    std::filesystem::create_directories(root);

    auto metadata = std::make_shared<MemoryMetadataStore>();

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

    nas::NasUser disabled;
    disabled.id = "disabled-id";
    disabled.username = "disabled";
    disabled.password_hash = "plain:disabled-secret";
    disabled.enabled = false;
    metadata->upsert_user(disabled);

    nas::NasUser hashed;
    hashed.id = "hashed-id";
    hashed.username = "hashed";
    hashed.password_hash = nas::NasAuthService::hash_password_for_config("hashed-secret", "salt");
    hashed.enabled = true;
    metadata->upsert_user(hashed);

    nas::NasUser nt_hash_user;
    nt_hash_user.id = "nthash-id";
    nt_hash_user.username = "nthash";
    nt_hash_user.password_hash = nas::NasAuthService::nt_hash_for_config("nthash-secret");
    nt_hash_user.enabled = true;
    metadata->upsert_user(nt_hash_user);

    auto share = make_share(root);
    metadata->upsert_share(share);

    auto smb_config = yuan::server::make_smb_config_from_nas(share.enabled ? nas::NasConfig{} : nas::NasConfig{}, metadata);
    check(smb_config.shares.size() == 1, "NAS share should become one SMB share");
    check(smb_config.shares[0].name == "docs", "SMB share name should match NAS share");
    check(smb_config.shares[0].path == root, "SMB share root should match NAS root");

    yuan::server::NasSmbHandler handler(metadata);
    smb::SmbSession reader_session(1, nullptr);
    check(handler.on_authenticate(&reader_session, "reader", "WORKGROUP"), "enabled NAS user should authenticate for SMB");
    check(!handler.on_authenticate(&reader_session, "disabled", "WORKGROUP"), "disabled NAS user should be rejected");
    auto reader_password = handler.on_password_lookup(&reader_session, "reader", "WORKGROUP");
    check(reader_password && *reader_password == "reader-secret", "plain NAS password should be available for NTLMv2 proof");
    check(!handler.on_password_lookup(&reader_session, "hashed", "WORKGROUP"),
          "non-plain NAS password hash should not be exposed for SMB NTLM proof");
    auto reader_nt_hash = handler.on_nt_hash_lookup(&reader_session, "nthash", "WORKGROUP");
    check(reader_nt_hash && *reader_nt_hash == nt_hash_user.password_hash,
          "NT hash NAS password should be available for SMB NTLM proof without exposing plain password");
    check(!handler.on_nt_hash_lookup(&reader_session, "reader", "WORKGROUP"),
          "plain NAS password should not require NT hash lookup path");

    smb::SmbShareConfig smb_share_config;
    smb_share_config.name = "docs";
    smb_share_config.path = root;
    smb::SmbShare smb_share(smb_share_config);
    smb::TreeConnection tree;
    tree.share_name = "docs";
    tree.share = &smb_share;
    const auto tree_id = reader_session.add_tree_connection(std::move(tree));

    check(handler.on_tree_connect(&reader_session, "\\\\localhost\\docs"), "reader should connect to readable share");

    smb::CreateParams read_open;
    read_open.desired_access = desired_file_read_data;
    read_open.create_disposition = disposition_file_open;
    check(handler.on_create(&reader_session, tree_id, "hello.txt", read_open), "reader should open for read");

    smb::CreateParams write_open;
    write_open.desired_access = desired_file_write_data;
    write_open.create_disposition = disposition_file_open_if;
    check(!handler.on_create(&reader_session, tree_id, "hello.txt", write_open), "reader should not open for write");

    smb::SmbSession writer_session(2, nullptr);
    check(handler.on_authenticate(&writer_session, "writer", "WORKGROUP"), "writer should authenticate");
    smb::TreeConnection writer_tree;
    writer_tree.share_name = "docs";
    writer_tree.share = &smb_share;
    const auto writer_tree_id = writer_session.add_tree_connection(std::move(writer_tree));
    check(handler.on_create(&writer_session, writer_tree_id, "hello.txt", write_open), "writer should open for write");

    smb::FileId file_id = writer_session.allocate_file_id();
    smb::OpenFile open_file;
    open_file.file_id = file_id;
    open_file.tree_id = writer_tree_id;
    smb_share.add_open_file(file_id, open_file);
    check(handler.on_write(&writer_session, file_id, 0, 4), "writer should write open file");
    check(handler.on_read(&writer_session, file_id, 0, 4), "writer should read open file");
    check(handler.on_rename(&writer_session, file_id, "renamed.txt"), "writer should rename open file");
    check(handler.on_delete(&writer_session, file_id), "writer should delete open file");
    check(handler.on_lock(&writer_session, file_id), "writer should lock open file");
    check(handler.on_ioctl(&writer_session, file_id, fsctl_validate_negotiate_info),
          "writer should issue read-like ioctl");
    check(handler.on_ioctl(&writer_session, file_id, fsctl_set_reparse_point),
          "writer should issue write-like ioctl");
    check(!handler.on_ioctl(&writer_session, file_id, fsctl_unknown),
          "unknown ioctl should be denied by default");

    check(!handler.on_rename(&reader_session, file_id, "reader-renamed.txt"),
          "reader should not rename open file");
    check(!handler.on_delete(&reader_session, file_id),
          "reader should not delete open file");
    check(!handler.on_lock(&reader_session, file_id),
          "reader should not lock open file");
    check(handler.on_ioctl(&reader_session, file_id, fsctl_validate_negotiate_info),
          "reader should issue read-like ioctl");
    check(!handler.on_ioctl(&reader_session, file_id, fsctl_set_reparse_point),
          "reader should not issue write-like ioctl");
    check(!handler.on_ioctl(&reader_session, file_id, fsctl_unknown),
          "reader should not issue unknown ioctl");

    handler.set_ioctl_permission(fsctl_validate_negotiate_info, nas::NasPermission::write);
    check(!handler.on_ioctl(&reader_session, file_id, fsctl_validate_negotiate_info),
          "reader should be denied when ioctl policy requires write");
    check(handler.on_ioctl(&writer_session, file_id, fsctl_validate_negotiate_info),
          "writer should pass when ioctl policy requires write");

    handler.set_ioctl_permission(fsctl_unknown, nas::NasPermission::read);
    check(handler.on_ioctl(&reader_session, file_id, fsctl_unknown),
          "reader should pass when unknown ioctl is explicitly mapped to read");

    handler.set_ioctl_permission(fsctl_set_reparse_point, std::nullopt);
    check(!handler.on_ioctl(&writer_session, file_id, fsctl_set_reparse_point),
          "writer should be denied when write-like ioctl is explicitly disabled");

    handler.reset_ioctl_permissions();
    check(handler.on_ioctl(&reader_session, file_id, fsctl_validate_negotiate_info),
          "reader should regain default read-like ioctl permission after reset");
    check(!handler.on_ioctl(&reader_session, file_id, fsctl_unknown),
          "reader should lose unknown ioctl mapping after reset");

    handler.on_logoff(&writer_session);
    check(!handler.on_write(&writer_session, file_id, 0, 4), "logged-off writer should lose SMB NAS permissions");
    check(!handler.on_rename(&writer_session, file_id, "after-logoff.txt"),
          "logged-off writer should lose SMB rename permission");
    check(!handler.on_delete(&writer_session, file_id),
          "logged-off writer should lose SMB delete permission");
    check(!handler.on_lock(&writer_session, file_id),
          "logged-off writer should lose SMB lock permission");
    check(!handler.on_ioctl(&writer_session, file_id, fsctl_validate_negotiate_info),
          "logged-off writer should lose SMB ioctl permission");

    std::filesystem::remove_all(root);
    std::cout << "NAS SMB adapter tests passed" << std::endl;
    return 0;
}
