#include "smb_dispatcher.h"
#include "crypto/smb_crypto_openssl.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <filesystem>

namespace yuan::net::smb
{
    namespace
    {
        void write_le32_at(ByteBuffer &buf, size_t offset, uint32_t value)
        {
            if (offset + 4 > buf.write_offset()) {
                return;
            }
            uint8_t le[4] = {
                static_cast<uint8_t>(value & 0xFF),
                static_cast<uint8_t>((value >> 8) & 0xFF),
                static_cast<uint8_t>((value >> 16) & 0xFF),
                static_cast<uint8_t>((value >> 24) & 0xFF)
            };
            std::memcpy(buf.data() + offset, le, sizeof(le));
        }

        void append_utf16le(ByteBuffer &buf, const std::u16string &text)
        {
            for (char16_t c : text) {
                Smb2Codec::write_le16(buf, static_cast<uint16_t>(c));
            }
        }

        void append_directory_entry(ByteBuffer &buf, const DirEntry &entry, FileInfoClass info_class)
        {
            const uint32_t name_len = static_cast<uint32_t>(entry.file_name.size() * 2);
            Smb2Codec::write_le32(buf, 0);
            Smb2Codec::write_le32(buf, 0);
            Smb2Codec::write_le64(buf, entry.creation_time);
            Smb2Codec::write_le64(buf, entry.last_access_time);
            Smb2Codec::write_le64(buf, entry.last_write_time);
            Smb2Codec::write_le64(buf, entry.change_time);
            Smb2Codec::write_le64(buf, entry.end_of_file);
            Smb2Codec::write_le64(buf, entry.allocation_size);
            Smb2Codec::write_le32(buf, entry.file_attributes);
            Smb2Codec::write_le32(buf, name_len);

            switch (info_class) {
            case FileInfoClass::FileFullDirectoryInformation:
                Smb2Codec::write_le32(buf, 0);
                break;
            case FileInfoClass::FileBothDirectoryInformation:
                Smb2Codec::write_le32(buf, 0);
                buf.append_u8(0);
                buf.append_u8(0);
                for (size_t i = 0; i < 24; ++i) {
                    buf.append_u8(0);
                }
                break;
            case FileInfoClass::FileIdFullDirectoryInformation:
                Smb2Codec::write_le32(buf, 0);
                for (size_t i = 0; i < 4; ++i) {
                    buf.append_u8(0);
                }
                Smb2Codec::write_le64(buf, entry.file_id.persistent);
                break;
            case FileInfoClass::FileIdBothDirectoryInformation:
                Smb2Codec::write_le32(buf, 0);
                buf.append_u8(0);
                buf.append_u8(0);
                for (size_t i = 0; i < 24; ++i) {
                    buf.append_u8(0);
                }
                buf.append_u8(0);
                buf.append_u8(0);
                Smb2Codec::write_le64(buf, entry.file_id.persistent);
                break;
            case FileInfoClass::FileDirectoryInformation:
            default:
                break;
            }

            append_utf16le(buf, entry.file_name);
        }

        void append_utf16le_string(ByteBuffer &buf, const std::string &text)
        {
            append_utf16le(buf, Smb2Codec::utf8_to_utf16le(text));
        }

        std::vector<uint8_t> buffer_to_vector(ByteBuffer &buf)
        {
            auto span = buf.readable_span();
            return std::vector<uint8_t>(
                reinterpret_cast<const uint8_t *>(span.data()),
                reinterpret_cast<const uint8_t *>(span.data()) + span.size());
        }

        std::vector<uint8_t> build_filesystem_info(const SmbShare &share, uint8_t file_info_class)
        {
            constexpr uint32_t bytes_per_sector = 4096;
            constexpr uint32_t sectors_per_unit = 1;
            constexpr uint32_t fs_attrs = FILE_CASE_SENSITIVE_SEARCH |
                                          FILE_CASE_PRESERVED_NAMES |
                                          FILE_UNICODE_ON_DISK |
                                          FILE_SUPPORTS_SPARSE_FILES |
                                          FILE_SUPPORTS_REPARSE_POINTS |
                                          FILE_SUPPORTS_EXTENDED_ATTRIBUTES;

            std::error_code ec;
            auto space = std::filesystem::space(share.path().empty() ? "." : share.path(), ec);
            uint64_t total_units = 0;
            uint64_t available_units = 0;
            if (!ec && space.capacity != static_cast<uintmax_t>(-1)) {
                total_units = static_cast<uint64_t>(space.capacity / bytes_per_sector);
                available_units = static_cast<uint64_t>(space.available / bytes_per_sector);
            }

            ByteBuffer buf(128);
            switch (static_cast<FileInfoClass>(file_info_class)) {
            case FileInfoClass::FileFsVolumeInformation: {
                const std::string label = share.name().empty() ? "YUAN" : share.name();
                Smb2Codec::write_le64(buf, Smb2Codec::filetime_now());
                Smb2Codec::write_le32(buf, static_cast<uint32_t>(std::hash<std::string>{}(share.name())));
                Smb2Codec::write_le32(buf, static_cast<uint32_t>(label.size() * 2));
                buf.append_u8(0);
                buf.append_u8(0);
                append_utf16le_string(buf, label);
                return buffer_to_vector(buf);
            }
            case FileInfoClass::FileFsSizeInformation:
                Smb2Codec::write_le64(buf, total_units);
                Smb2Codec::write_le64(buf, available_units);
                Smb2Codec::write_le32(buf, sectors_per_unit);
                Smb2Codec::write_le32(buf, bytes_per_sector);
                return buffer_to_vector(buf);
            case FileInfoClass::FileFsDeviceInformation:
                Smb2Codec::write_le32(buf, share.type() == ShareType::PIPE ? FILE_DEVICE_NAMED_PIPE : FILE_DEVICE_DISK);
                Smb2Codec::write_le32(buf, FILE_DEVICE_SECURE_OPEN);
                return buffer_to_vector(buf);
            case FileInfoClass::FileFsAttributeInformation: {
                const std::string fs_name = "NTFS";
                Smb2Codec::write_le32(buf, fs_attrs);
                Smb2Codec::write_le32(buf, 255);
                Smb2Codec::write_le32(buf, static_cast<uint32_t>(fs_name.size() * 2));
                append_utf16le_string(buf, fs_name);
                return buffer_to_vector(buf);
            }
            case FileInfoClass::FileFsFullSizeInformation:
                Smb2Codec::write_le64(buf, total_units);
                Smb2Codec::write_le64(buf, available_units);
                Smb2Codec::write_le64(buf, available_units);
                Smb2Codec::write_le32(buf, sectors_per_unit);
                Smb2Codec::write_le32(buf, bytes_per_sector);
                return buffer_to_vector(buf);
            case FileInfoClass::FileFsSectorSizeInformation:
                Smb2Codec::write_le32(buf, bytes_per_sector);
                Smb2Codec::write_le32(buf, bytes_per_sector);
                Smb2Codec::write_le32(buf, bytes_per_sector);
                Smb2Codec::write_le32(buf, bytes_per_sector);
                Smb2Codec::write_le32(buf, 0);
                Smb2Codec::write_le32(buf, 0);
                Smb2Codec::write_le32(buf, 0);
                return buffer_to_vector(buf);
            default:
                return {};
            }
        }

        std::vector<uint8_t> build_validate_negotiate_info(const SmbSession &session,
                                                           uint32_t capabilities,
                                                           uint16_t security_mode,
                                                           const uint8_t server_guid[16])
        {
            ByteBuffer buf(24);
            Smb2Codec::write_le32(buf, capabilities);
            buf.append(server_guid, 16);
            Smb2Codec::write_le16(buf, security_mode);
            Smb2Codec::write_le16(buf, static_cast<uint16_t>(session.dialect()));
            return buffer_to_vector(buf);
        }

        bool validate_negotiate_info_request(const SmbSession &session,
                                             const std::vector<uint8_t> &input)
        {
            if (input.size() < 24) {
                return false;
            }

            const uint16_t dialect_count = Smb2Codec::read_le16(input.data() + 22);
            if (input.size() < 24u + static_cast<size_t>(dialect_count) * 2u) {
                return false;
            }

            for (uint16_t i = 0; i < dialect_count; ++i) {
                const uint16_t dialect = Smb2Codec::read_le16(input.data() + 24u + i * 2u);
                if (dialect == static_cast<uint16_t>(session.dialect())) {
                    return true;
                }
            }
            return false;
        }

        std::vector<uint8_t> build_network_interface_info()
        {
            ByteBuffer buf(152);
            Smb2Codec::write_le32(buf, 0);
            Smb2Codec::write_le32(buf, 1);
            Smb2Codec::write_le32(buf, SMB2_NETWORK_INTERFACE_RSS_CAPABLE);
            Smb2Codec::write_le32(buf, 0);
            Smb2Codec::write_le64(buf, 1000000000ULL);

            Smb2Codec::write_le16(buf, 2);
            Smb2Codec::write_le16(buf, 0);
            buf.append_u8(127);
            buf.append_u8(0);
            buf.append_u8(0);
            buf.append_u8(1);
            for (size_t i = 0; i < 120; ++i) {
                buf.append_u8(0);
            }

            return buffer_to_vector(buf);
        }

        std::optional<std::string> parse_rename_target_from_set_info(const std::vector<uint8_t> &buffer)
        {
            if (buffer.size() < 20) {
                return std::nullopt;
            }
            const uint32_t name_len = Smb2Codec::read_le32(buffer.data() + 16);
            if ((name_len % 2) != 0 || 20u + name_len > buffer.size()) {
                return std::nullopt;
            }

            std::u16string new_name;
            new_name.resize(name_len / 2);
            for (size_t i = 0; i < new_name.size(); ++i) {
                new_name[i] = static_cast<char16_t>(Smb2Codec::read_le16(buffer.data() + 20 + i * 2));
            }
            return Smb2Codec::utf16le_to_utf8(new_name);
        }
    }

    SmbDispatcher::SmbDispatcher(const SmbServerConfig & config,
                                 SmbShareManager & share_mgr,
                                 SmbLockManager & lock_mgr,
                                 SmbPipeManager & pipe_mgr,
                                 SmbDfsResolver & dfs_resolver,
                                 SmbChangeNotifier & change_notifier,
                                 SmbHandler * handler)
        : config_(config), share_mgr_(share_mgr), lock_mgr_(lock_mgr), pipe_mgr_(pipe_mgr), dfs_resolver_(dfs_resolver), change_notifier_(change_notifier), handler_(handler)
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        for (int i = 0; i < 16; ++i) {
            server_guid_[i] = static_cast<uint8_t>(gen() & 0xFF);
        }
    }

    ByteBuffer SmbDispatcher::dispatch(SmbSession & session, const Smb2Header & header,
                                       const uint8_t * data, size_t len)
    {
        switch (static_cast<Smb2Command>(header.command)) {
        case Smb2Command::NEGOTIATE:
            return handle_negotiate(session, header, data, len);
        case Smb2Command::SESSION_SETUP:
            return handle_session_setup(session, header, data, len);
        case Smb2Command::LOGOFF:
            return handle_logoff(session, header, data, len);
        case Smb2Command::TREE_CONNECT:
            return handle_tree_connect(session, header, data, len);
        case Smb2Command::TREE_DISCONNECT:
            return handle_tree_disconnect(session, header, data, len);
        case Smb2Command::CREATE:
            return handle_create(session, header, data, len);
        case Smb2Command::CLOSE:
            return handle_close(session, header, data, len);
        case Smb2Command::READ:
            return handle_read(session, header, data, len);
        case Smb2Command::WRITE:
            return handle_write(session, header, data, len);
        case Smb2Command::QUERY_DIRECTORY:
            return handle_query_directory(session, header, data, len);
        case Smb2Command::QUERY_INFO:
            return handle_query_info(session, header, data, len);
        case Smb2Command::SET_INFO:
            return handle_set_info(session, header, data, len);
        case Smb2Command::LOCK:
            return handle_lock(session, header, data, len);
        case Smb2Command::IOCTL:
            return handle_ioctl(session, header, data, len);
        case Smb2Command::ECHO:
            return handle_echo(session, header, data, len);
        case Smb2Command::FLUSH:
            return handle_flush(session, header, data, len);
        case Smb2Command::CHANGE_NOTIFY:
            return handle_change_notify(session, header, data, len);
        case Smb2Command::CANCEL:
            return handle_cancel(session, header, data, len);
        case Smb2Command::OPLOCK_BREAK:
            return handle_oplock_break(session, header, data, len);
        default:
            return make_error(header, NtStatus::NOT_IMPLEMENTED);
        }
    }

    std::vector<ByteBuffer> SmbDispatcher::dispatch_compound(SmbSession & session,
                                                             const uint8_t * data, size_t len)
    {
        std::vector<ByteBuffer> responses;
        size_t offset = 0;

        while (offset + SMB2_HEADER_SIZE <= len) {
            auto header = Smb2Codec::decode_header(data + offset, len - offset);
            if (!header)
                break;

            size_t cmd_len = len - offset;
            if (header->next_command > 0) {
                cmd_len = header->next_command;
            }

            auto resp = dispatch(session, *header, data + offset, cmd_len);
            responses.push_back(std::move(resp));

            if (header->next_command == 0)
                break;
            offset += header->next_command;
        }

        for (size_t i = 0; i < responses.size(); ++i) {
            uint32_t next_cmd = 0;
            if (i + 1 < responses.size()) {
                next_cmd = static_cast<uint32_t>(responses[i].readable_bytes());
            }
            char *base = responses[i].read_ptr();
            uint8_t le[4] = {
                static_cast<uint8_t>(next_cmd & 0xFF),
                static_cast<uint8_t>((next_cmd >> 8) & 0xFF),
                static_cast<uint8_t>((next_cmd >> 16) & 0xFF),
                static_cast<uint8_t>((next_cmd >> 24) & 0xFF)
            };
            std::memcpy(base + 20, le, 4);
        }

        return responses;
    }

     ByteBuffer SmbDispatcher::handle_negotiate(SmbSession & session, const Smb2Header & header,
                                                 const uint8_t * data, size_t len)
     {
         auto req = Smb2Codec::decode_negotiate_request(data, len);
         if (!req)
             return make_error(header, NtStatus::INVALID_PARAMETER);

         DialectRevision dialect = select_dialect(req->dialects);
         session.set_dialect(dialect);
         session.set_state(SmbSession::State::negotiating);

          if (dialect >= DialectRevision::SMB_3_1_1) {
              session.set_preauth_hash(std::vector<uint8_t>(64, 0));
              auto hash = SmbKeyDerivation::compute_preauth_hash(session.preauth_hash(), data, len);
              session.set_preauth_hash(std::move(hash));
          }

         uint32_t caps = compute_server_capabilities(dialect);
         uint16_t sec_mode = compute_security_mode();
         session.set_server_capabilities(caps);
         session.set_server_security_mode(sec_mode);

         Smb2NegotiateResponse resp;
         std::memcpy(resp.server_guid, server_guid_, 16);
         resp.dialect_revision = static_cast<uint16_t>(dialect);
         resp.security_mode = sec_mode;
         resp.capabilities = caps;
         resp.max_transact_size = config_.max_transact_size;
         resp.max_read_size = config_.max_read_size;
         resp.max_write_size = config_.max_write_size;
         resp.system_time = Smb2Codec::filetime_now();
         resp.server_start_time = resp.system_time;

         if (dialect >= DialectRevision::SMB_3_1_1 && crypto_) {
             std::vector<NegotiateContext> ctxs;

              NegotiateContext preauth_ctx;
              preauth_ctx.context_type = SMB2_NEGOTIATE_CTX_PREAUTH_INTEGRITY;
             ByteBuffer preauth_buf(64);
             Smb2Codec::write_le16(preauth_buf, 1);
             Smb2Codec::write_le16(preauth_buf, 32);
             Smb2Codec::write_le16(preauth_buf, static_cast<uint16_t>(SMB2_PREAUTH_INTEGRITY_CAP_SHA_512));
             for (int i = 0; i < 32; ++i) {
                 preauth_buf.append_u8(static_cast<uint8_t>(i + 1));
             }
             preauth_ctx.data = buffer_to_vector(preauth_buf);
             ctxs.push_back(preauth_ctx);

              if (config_.enable_encryption) {
                  NegotiateContext enc_ctx;
                  enc_ctx.context_type = SMB2_NEGOTIATE_CTX_ENCRYPTION;
                  ByteBuffer enc_buf(16);
                  Smb2Codec::write_le16(enc_buf, 2);
                  Smb2Codec::write_le16(enc_buf, static_cast<uint16_t>(EncryptionAlgorithm::AES_128_GCM));
                  Smb2Codec::write_le16(enc_buf, static_cast<uint16_t>(EncryptionAlgorithm::AES_128_CCM));
                  enc_ctx.data = buffer_to_vector(enc_buf);
                  ctxs.push_back(enc_ctx);
              }

              NegotiateContext signing_ctx;
              signing_ctx.context_type = SMB2_NEGOTIATE_CTX_SIGNING;
              ByteBuffer signing_buf(8);
              Smb2Codec::write_le16(signing_buf, 1);
              Smb2Codec::write_le16(signing_buf, SMB2_SIGNING_AES128_CMAC);
              signing_ctx.data = buffer_to_vector(signing_buf);
              ctxs.push_back(signing_ctx);

              session.set_signing_algorithm(SMB2_SIGNING_AES128_CMAC);

              auto ctx_buf = Smb2Codec::encode_negotiate_contexts(ctxs);
             auto ctx_span = ctx_buf.readable_span();
             resp.negotiate_context.assign(
                 reinterpret_cast<const uint8_t *>(ctx_span.data()),
                 reinterpret_cast<const uint8_t *>(ctx_span.data()) + ctx_span.size());
             resp.negotiate_context_count = static_cast<uint16_t>(ctxs.size());
         }

         auto resp_buf = Smb2Codec::encode_negotiate_response(header, resp);

          if (dialect >= DialectRevision::SMB_3_1_1) {
              auto span = resp_buf.readable_span();
              auto hash = SmbKeyDerivation::compute_preauth_hash(
                  session.preauth_hash(),
                  reinterpret_cast<const uint8_t *>(span.data()),
                  span.size());
              session.set_preauth_hash(std::move(hash));
          }

         return resp_buf;
     }

      ByteBuffer SmbDispatcher::handle_session_setup(SmbSession & session, const Smb2Header & header,
                                                      const uint8_t * data, size_t len)
      {
          auto req = Smb2Codec::decode_session_setup_request(data, len);
         if (!req)
             return make_error(header, NtStatus::INVALID_PARAMETER);

          bool is_smb311 = session.dialect() >= DialectRevision::SMB_3_1_1;

           if (is_smb311) {
               auto hash = SmbKeyDerivation::compute_preauth_hash(session.preauth_hash(), data, len);
               session.set_preauth_hash(std::move(hash));
           }

         if (!session.auth()) {
             auto auth = std::make_unique<SmbSpnegoAuth>(config_.server_name, config_.domain_name);
             if (handler_) {
                 auth->set_credentials_db(
                     [this, &session](const std::string & u, const std::string & d, const std::string & p)->bool {
                         (void)p;
                         return handler_->on_authenticate(&session, u, d);
                     });
                 auth->set_password_lookup(
                     [this, &session](const std::string & u, const std::string & d)->std::optional<std::string> {
                         return handler_->on_password_lookup(&session, u, d);
                     });
                 auth->set_nt_hash_lookup(
                     [this, &session](const std::string & u, const std::string & d)->std::optional<std::string> {
                         return handler_->on_nt_hash_lookup(&session, u, d);
                     });
             }
             session.set_auth(std::move(auth));
         }

         auto outbound = session.auth()->process_inbound_token(req->security_buffer);

         Smb2SessionSetupResponse resp;
         resp.security_buffer = std::move(outbound);

         if (session.auth()->is_complete()) {
             const auto &result = session.auth()->result();
             if (result.success) {
                 resp.session_flags = 0;
                 session.set_state(SmbSession::State::authenticated);
                 session.set_user_name(result.user_name);
                 session.set_domain_name(result.domain_name);

                 auto resp_hdr = header;
                 resp_hdr.session_id = session.session_id();
                 auto resp_buf = Smb2Codec::encode_session_setup_response(resp_hdr, resp);

                 if (is_smb311) {
                 }

                  derive_session_keys(session, result.session_key);

                 if (handler_) {
                     handler_->on_session_opened(&session);
                 }

                 return resp_buf;
             } else {
                 return make_error(header, NtStatus::LOGON_FAILURE);
             }
         } else {
             auto err_hdr = header;
             err_hdr.status = static_cast<uint32_t>(NtStatus::MORE_PROCESSING_REQUIRED);
             err_hdr.session_id = session.session_id();
             auto resp_buf = Smb2Codec::encode_session_setup_response(err_hdr, resp);

              if (is_smb311) {
                  auto span = resp_buf.readable_span();
                  auto hash = SmbKeyDerivation::compute_preauth_hash(
                      session.preauth_hash(),
                      reinterpret_cast<const uint8_t *>(span.data()),
                      span.size());
                  session.set_preauth_hash(std::move(hash));
              }

             return resp_buf;
         }
     }

    ByteBuffer SmbDispatcher::handle_logoff(SmbSession & session, const Smb2Header & header,
                                            const uint8_t * data, size_t len)
    {
        if (handler_) {
            handler_->on_logoff(&session);
        }

        auto tree_ids = session.all_tree_ids();
        for (uint32_t tid : tree_ids) {
            session.remove_tree(tid);
        }

        session.set_state(SmbSession::State::connected);

        Smb2LogoffResponse resp;
        return Smb2Codec::encode_logoff_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_tree_connect(SmbSession & session, const Smb2Header & header,
                                                  const uint8_t * data, size_t len)
    {
        if (session.state() < SmbSession::State::authenticated) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        auto req = Smb2Codec::decode_tree_connect_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        std::string path = Smb2Codec::utf16le_to_utf8(req->path);

        std::string share_name;
        size_t last_sep = path.find_last_of("\\/");
        if (last_sep != std::string::npos) {
            share_name = path.substr(last_sep + 1);
        } else {
            share_name = path;
        }

        SmbShare *share = share_mgr_.find_share(share_name);
        if (!share) {
            auto referral = dfs_resolver_.resolve(path);
            if (referral) {
                return make_error(header, NtStatus::NOT_SUPPORTED);
            }
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);
        }

        if (handler_ && !handler_->on_tree_connect(&session, path)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        TreeConnection tree;
        tree.share_name = share_name;
        tree.share = share;
        tree.is_dfs = (share->share_flags() & SMB2_SHAREFLAG_DFS) != 0;
        tree.is_ca = (share->capabilities() & SMB2_GLOBAL_CAP_PERSISTENT_HANDLES) != 0;
        uint32_t tree_id = session.add_tree_connection(std::move(tree));

        Smb2TreeConnectResponse resp;
        resp.share_type = static_cast<uint8_t>(share->type());
        resp.share_flags = share->share_flags();
        resp.capabilities = share->capabilities();
        resp.maximal_access = SMB_FILE_GENERIC_READ | SMB_FILE_GENERIC_WRITE | SMB_FILE_GENERIC_EXECUTE;

        share->increment_uses();
        session.set_state(SmbSession::State::active);

        auto hdr = Smb2Codec::make_response_header(header, static_cast<uint16_t>(Smb2Command::TREE_CONNECT));
        hdr.tree_id = tree_id;
        return Smb2Codec::encode_tree_connect_response(hdr, resp);
    }

    ByteBuffer SmbDispatcher::handle_tree_disconnect(SmbSession & session, const Smb2Header & header,
                                                     const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_tree_disconnect_request(data, len);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (tree) {
            if (handler_) {
                handler_->on_tree_disconnect(&session, header.tree_id);
            }

            auto file_ids = tree->share->all_open_file_ids();
            for (const auto &fid : file_ids) {
                tree->share->remove_open_file(fid);
            }

            tree->share->decrement_uses();
            session.remove_tree(header.tree_id);
        }

        Smb2TreeDisconnectResponse resp;
        return Smb2Codec::encode_tree_disconnect_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_create(SmbSession & session, const Smb2Header & header,
                                            const uint8_t * data, size_t len)
    {
        if (session.state() < SmbSession::State::active) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        auto req = Smb2Codec::decode_create_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        std::string file_path = Smb2Codec::utf16le_to_utf8(req->buffer);

        if (tree->share->type() == ShareType::PIPE) {
            if (handler_ && !handler_->on_pipe_open(&session, file_path)) {
                return make_error(header, NtStatus::ACCESS_DENIED);
            }
            uint64_t pipe_handle = pipe_mgr_.open_pipe(file_path, session.session_id());

            Smb2CreateResponse resp;
            resp.create_action = SMB_FILE_OPENED;
            resp.oplock_level = SMB2_OPLOCK_LEVEL_NONE;
            resp.file_id = session.allocate_file_id();

            OpenFile of;
            of.file_id = resp.file_id;
            of.path = file_path;
            of.is_directory = false;
            of.oplock_level = SMB2_OPLOCK_LEVEL_NONE;
            of.file_handle = reinterpret_cast<void *>(pipe_handle);
            of.tree_id = header.tree_id;
            tree->share->add_open_file(resp.file_id, std::move(of));

            return Smb2Codec::encode_create_response(header, resp);
        }

        SmbFileSystem *fs = tree->share->file_system();
        if (!fs)
            return make_error(header, NtStatus::INTERNAL_ERROR);

        if (handler_ && !handler_->on_create(&session, header.tree_id, file_path,
                                             { req->desired_access, req->file_attributes,
                                               req->share_access, req->create_disposition,
                                               req->create_options })) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        auto open_result = fs->open(file_path, req->desired_access,
                                    req->create_disposition, req->create_options);
        if (!open_result.success) {
            return make_error(header, open_result.status);
        }

        uint8_t oplock_level = SMB2_OPLOCK_LEVEL_NONE;
        auto contexts = Smb2Codec::parse_create_contexts(req->create_contexts);
        for (const auto &ctx : contexts) {
            std::string name(ctx.name.begin(), ctx.name.end());
            if (name == "RqLs" && ctx.data.size() >= 4) {
                uint32_t req_state = Smb2Codec::read_le32(ctx.data.data());
                auto status = lock_mgr_.request_lease(
                    session.allocate_file_id(), session.session_id(),
                    ctx.data.data() + 4, req_state);
                if (status == NtStatus::SUCCESS) {
                    oplock_level = 0xFF;
                }
            } else if (name == "DHnQ" || name == "DHnC") {
            }
        }

        FileId file_id = session.allocate_file_id();

        Smb2CreateResponse resp;
        resp.oplock_level = oplock_level;
        resp.create_action = open_result.create_action;
        resp.creation_time = open_result.creation_time;
        resp.last_access_time = open_result.last_access_time;
        resp.last_write_time = open_result.last_write_time;
        resp.change_time = open_result.change_time;
        resp.allocation_size = open_result.allocation_size;
        resp.end_of_file = open_result.end_of_file;
        resp.file_attributes = open_result.file_attributes;
        resp.file_id = file_id;

        OpenFile of;
        of.file_id = file_id;
        of.path = file_path;
        of.access_mask = req->desired_access;
        of.share_access = req->share_access;
        of.create_disposition = req->create_disposition;
        of.file_attributes = req->file_attributes;
        of.is_directory = open_result.is_directory;
        of.oplock_level = oplock_level;
        of.file_handle = open_result.handle;
        of.tree_id = header.tree_id;
        tree->share->add_open_file(file_id, std::move(of));

        return Smb2Codec::encode_create_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_close(SmbSession & session, const Smb2Header & header,
                                           const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_close_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);

        Smb2CloseResponse resp;

        if (of) {
            if (handler_) {
                handler_->on_close(&session, req->file_id);
            }

            if (tree->share->type() == ShareType::PIPE) {
                uint64_t pipe_handle = reinterpret_cast<uint64_t>(of->file_handle);
                pipe_mgr_.close_pipe(pipe_handle);
            } else if (tree->share->file_system() && of->file_handle) {
                tree->share->file_system()->close(of->file_handle);
            }

            lock_mgr_.release_all_locks(req->file_id, session.session_id());
            lock_mgr_.remove_oplock(req->file_id);
            lock_mgr_.remove_lease(req->file_id);

            resp.creation_time = 0;
            resp.last_access_time = 0;
            resp.last_write_time = 0;
            resp.change_time = 0;
            resp.allocation_size = 0;
            resp.end_of_file = 0;
            resp.file_attributes = 0;

            tree->share->remove_open_file(req->file_id);
        }

        return Smb2Codec::encode_close_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_read(SmbSession & session, const Smb2Header & header,
                                          const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_read_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        if (handler_ && !handler_->on_read(&session, req->file_id, req->offset, req->length)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        Smb2ReadResponse resp;
        resp.data_offset = SMB2_HEADER_SIZE + 16;

        if (tree->share->type() == ShareType::PIPE) {
            uint64_t pipe_handle = reinterpret_cast<uint64_t>(of->file_handle);
            resp.buffer = pipe_mgr_.read_pipe(pipe_handle, req->length);
            resp.data_length = static_cast<uint32_t>(resp.buffer.size());
        } else {
            SmbFileSystem *fs = tree->share->file_system();
            if (!fs || !of->file_handle)
                return make_error(header, NtStatus::INTERNAL_ERROR);

            auto result = fs->read(of->file_handle, req->offset, req->length);
            if (!result.success) {
                return make_error(header, result.status);
            }
            resp.buffer = std::move(result.data);
            resp.data_length = result.bytes_read;
        }

        return Smb2Codec::encode_read_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_write(SmbSession & session, const Smb2Header & header,
                                           const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_write_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        if (handler_ && !handler_->on_write(&session, req->file_id, req->offset, req->length)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        Smb2WriteResponse resp;

        if (tree->share->type() == ShareType::PIPE) {
            uint64_t pipe_handle = reinterpret_cast<uint64_t>(of->file_handle);
            resp.count = pipe_mgr_.write_pipe(pipe_handle, req->buffer.data(),
                                              static_cast<uint32_t>(req->buffer.size()));
        } else {
            SmbFileSystem *fs = tree->share->file_system();
            if (!fs || !of->file_handle)
                return make_error(header, NtStatus::INTERNAL_ERROR);

            auto result = fs->write(of->file_handle, req->offset,
                                    req->buffer.data(), static_cast<uint32_t>(req->buffer.size()));
            if (!result.success) {
                return make_error(header, result.status);
            }
            resp.count = result.bytes_written;
        }

        return Smb2Codec::encode_write_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_query_directory(SmbSession & session, const Smb2Header & header,
                                                     const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_query_directory_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        if (handler_ && !handler_->on_query_directory(&session, req->file_id)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        SmbFileSystem *fs = tree->share->file_system();
        if (!fs || !of->file_handle)
            return make_error(header, NtStatus::INTERNAL_ERROR);

        bool restart = (req->flags & SL_RESTART_SCAN) != 0;
        const bool single_entry = (req->flags & SL_RETURN_SINGLE_ENTRY) != 0;
        std::string pattern = Smb2Codec::utf16le_to_utf8(req->file_name);
        auto entries = fs->query_directory(of->file_handle, pattern,
                                           static_cast<FileInfoClass>(req->file_information_class),
                                           restart, single_entry ? 1u : 0u);
        if (!entries) {
            return make_error(header, NtStatus::NO_MORE_FILES);
        }

        Smb2QueryDirectoryResponse resp;
        resp.output_buffer_offset = SMB2_HEADER_SIZE + 8;
        ByteBuffer dir_buf(req->output_buffer_length);
        size_t previous_entry_start = 0;
        bool has_previous_entry = false;

        for (const auto &entry : *entries) {
            size_t entry_start = dir_buf.write_offset();
            append_directory_entry(dir_buf, entry, static_cast<FileInfoClass>(req->file_information_class));
            size_t pad = (4 - (dir_buf.write_offset() & 3)) & 3;
            for (size_t p = 0; p < pad; ++p)
                dir_buf.append_u8(0);

            if (dir_buf.write_offset() - entry_start > req->output_buffer_length) {
                dir_buf.set_write_offset(entry_start);
                break;
            }

            if (has_previous_entry) {
                write_le32_at(dir_buf, previous_entry_start,
                              static_cast<uint32_t>(entry_start - previous_entry_start));
            }
            previous_entry_start = entry_start;
            has_previous_entry = true;

            if (single_entry) {
                break;
            }
        }

        resp.output_buffer_length = static_cast<uint32_t>(dir_buf.readable_bytes());
        resp.buffer = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t *>(dir_buf.readable_span().data()),
            reinterpret_cast<const uint8_t *>(dir_buf.readable_span().data()) + dir_buf.readable_bytes());

        return Smb2Codec::encode_query_directory_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_query_info(SmbSession & session, const Smb2Header & header,
                                                const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_query_info_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        if (handler_ && !handler_->on_query_info(&session, req->file_id)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        SmbFileSystem *fs = tree->share->file_system();
        if (!fs || !of->file_handle)
            return make_error(header, NtStatus::INTERNAL_ERROR);

        std::vector<uint8_t> info_buffer;
        if (req->info_type == SMB2_0_INFO_FILESYSTEM) {
            info_buffer = build_filesystem_info(*tree->share, req->file_info_class);
            if (info_buffer.empty()) {
                return make_error(header, NtStatus::NOT_SUPPORTED);
            }
        } else if (req->info_type == SMB2_0_INFO_FILE) {
            auto info_data = fs->query_info(of->file_handle,
                                            static_cast<FileInfoClass>(req->file_info_class));
            if (!info_data) {
                return make_error(header, NtStatus::NOT_SUPPORTED);
            }
            info_buffer = std::move(*info_data);
        } else {
            return make_error(header, NtStatus::NOT_SUPPORTED);
        }

        if (req->output_buffer_length > 0 && info_buffer.size() > req->output_buffer_length) {
            info_buffer.resize(req->output_buffer_length);
        }

        Smb2QueryInfoResponse resp;
        resp.output_buffer_offset = SMB2_HEADER_SIZE + 8;
        resp.output_buffer_length = static_cast<uint32_t>(info_buffer.size());
        resp.buffer = std::move(info_buffer);

        return Smb2Codec::encode_query_info_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_set_info(SmbSession & session, const Smb2Header & header,
                                              const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_set_info_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        if (handler_ && !handler_->on_set_info(&session, req->file_id)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        if (handler_ && req->info_type == SMB2_0_INFO_FILE) {
            const auto info_class = static_cast<FileInfoClass>(req->file_info_class);
            if (info_class == FileInfoClass::FileRenameInformation) {
                auto new_path = parse_rename_target_from_set_info(req->buffer);
                if (!new_path.has_value()) {
                    return make_error(header, NtStatus::INVALID_PARAMETER);
                }
                if (!handler_->on_rename(&session, req->file_id, *new_path)) {
                    return make_error(header, NtStatus::ACCESS_DENIED);
                }
            } else if (info_class == FileInfoClass::FileDispositionInformation) {
                if (!req->buffer.empty() && req->buffer[0] != 0 && !handler_->on_delete(&session, req->file_id)) {
                    return make_error(header, NtStatus::ACCESS_DENIED);
                }
            }
        }

        SmbFileSystem *fs = tree->share->file_system();
        if (!fs || !of->file_handle)
            return make_error(header, NtStatus::INTERNAL_ERROR);

        auto status = fs->set_info(of->file_handle,
                                   static_cast<FileInfoClass>(req->file_info_class),
                                   req->buffer.data(), static_cast<uint32_t>(req->buffer.size()));
        if (status != NtStatus::SUCCESS) {
            return make_error(header, status);
        }

        Smb2SetInfoResponse resp;
        return Smb2Codec::encode_set_info_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_lock(SmbSession & session, const Smb2Header & header,
                                          const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_lock_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        if (handler_ && !handler_->on_lock(&session, req->file_id)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        for (const auto &lk : req->locks) {
            bool exclusive = (lk.flags & 0x00000002) != 0;
            auto status = lock_mgr_.request_lock(req->file_id, session.session_id(),
                                                 header.tree_id, lk.offset, lk.length, exclusive);
            if (status != NtStatus::SUCCESS) {
                if (lk.flags & 0x00000001) {
                    return make_error(header, status);
                }
            }
        }

        Smb2LockResponse resp;
        return Smb2Codec::encode_lock_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_ioctl(SmbSession & session, const Smb2Header & header,
                                           const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_ioctl_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        if (handler_ && !handler_->on_ioctl(&session, req->file_id, req->ctl_code)) {
            return make_error(header, NtStatus::ACCESS_DENIED);
        }

        Smb2IoctlResponse resp;
        resp.ctl_code = req->ctl_code;
        resp.file_id = req->file_id;

        if (req->ctl_code == FSCTL_VALIDATE_NEGOTIATE_INFO) {
            if (!validate_negotiate_info_request(session, req->input_buffer)) {
                return make_error(header, NtStatus::ACCESS_DENIED);
            }
            resp.output_buffer = build_validate_negotiate_info(
                session, session.server_capabilities(), session.server_security_mode(), server_guid_);
        } else if (req->ctl_code == FSCTL_QUERY_NETWORK_INTERFACE_INFO) {
            resp.output_buffer = build_network_interface_info();
        } else if (req->ctl_code == FSCTL_LMR_REQUEST_RESILIENCY) {
            resp.output_buffer.clear();
        } else if (req->ctl_code == FSCTL_DFS_GET_REFERRALS || req->ctl_code == FSCTL_DFS_GET_REFERRALS_EX) {
            auto referral = dfs_resolver_.resolve(
                std::string(req->input_buffer.data(), req->input_buffer.data() + req->input_buffer.size()));
            if (referral) {
                if (handler_) {
                    auto resolved = handler_->on_dfs_resolve(&session, referral->dfs_path);
                }
                resp.output_buffer = { 0x00, 0x00, 0x00, 0x00 };
            } else {
                return make_error(header, NtStatus::NOT_SUPPORTED);
            }
        } else if (req->ctl_code == FSCTL_PIPE_TRANSCEIVE) {
            OpenFile *of = nullptr;
            TreeConnection *tree = session.find_tree(header.tree_id);
            if (tree) {
                of = tree->share->find_open_file(req->file_id);
            }
            if (of && tree && tree->share->type() == ShareType::PIPE) {
                uint64_t pipe_handle = reinterpret_cast<uint64_t>(of->file_handle);
                pipe_mgr_.write_pipe(pipe_handle, req->input_buffer.data(),
                                     static_cast<uint32_t>(req->input_buffer.size()));
                resp.output_buffer = pipe_mgr_.read_pipe(pipe_handle, req->max_output_response);
            } else {
                return make_error(header, NtStatus::INVALID_DEVICE_REQUEST);
            }
        } else {
            TreeConnection *tree = session.find_tree(header.tree_id);
            if (tree && tree->share->file_system()) {
                OpenFile *of = tree->share->find_open_file(req->file_id);
                if (of && of->file_handle) {
                    auto result = tree->share->file_system()->fsctl(
                        of->file_handle, req->ctl_code,
                        req->input_buffer.data(), static_cast<uint32_t>(req->input_buffer.size()));
                    if (!result.success) {
                        return make_error(header, result.status);
                    }
                    resp.output_buffer = std::move(result.output);
                } else {
                    return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);
                }
            } else {
                return make_error(header, NtStatus::NOT_SUPPORTED);
            }
        }

        resp.input_offset = 0;
        resp.input_length = 0;
        resp.output_offset = SMB2_HEADER_SIZE + 48;
        resp.output_length = static_cast<uint32_t>(resp.output_buffer.size());

        return Smb2Codec::encode_ioctl_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_echo(SmbSession & session, const Smb2Header & header,
                                          const uint8_t * data, size_t len)
    {
        Smb2EchoResponse resp;
        return Smb2Codec::encode_echo_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_flush(SmbSession & session, const Smb2Header & header,
                                           const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_flush_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        TreeConnection *tree = session.find_tree(header.tree_id);
        if (!tree)
            return make_error(header, NtStatus::NETWORK_NAME_DELETED);

        OpenFile *of = tree->share->find_open_file(req->file_id);
        if (!of)
            return make_error(header, NtStatus::OBJECT_NAME_NOT_FOUND);

        SmbFileSystem *fs = tree->share->file_system();
        if (fs && of->file_handle) {
            auto status = fs->flush(of->file_handle);
            if (status != NtStatus::SUCCESS) {
                return make_error(header, status);
            }
        }

        Smb2FlushResponse resp;
        return Smb2Codec::encode_flush_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_change_notify(SmbSession & session, const Smb2Header & header,
                                                   const uint8_t * data, size_t len)
    {
        auto req = Smb2Codec::decode_change_notify_request(data, len);
        if (!req)
            return make_error(header, NtStatus::INVALID_PARAMETER);

        change_notifier_.register_watch(
            req->file_id, req->completion_filter,
            [&session, header](const FileId &, uint32_t) {
            });

        Smb2ChangeNotifyResponse resp;
        resp.output_buffer_offset = SMB2_HEADER_SIZE + 8;
        resp.output_buffer_length = 0;
        return Smb2Codec::encode_change_notify_response(header, resp);
    }

    ByteBuffer SmbDispatcher::handle_cancel(SmbSession & session, const Smb2Header & header,
                                            const uint8_t * data, size_t len)
    {
        return Smb2Codec::build_error_response(header, NtStatus::CANCELLED);
    }

    ByteBuffer SmbDispatcher::handle_oplock_break(SmbSession & session, const Smb2Header & header,
                                                  const uint8_t * data, size_t len)
    {
        auto ack = Smb2Codec::decode_oplock_break_ack(data, len);
        if (!ack) {
            auto lease_ack = Smb2Codec::decode_lease_break_ack(data, len);
            if (lease_ack) {
                auto status = lock_mgr_.ack_lease_break(lease_ack->lease_key,
                                                        session.session_id(),
                                                        lease_ack->lease_state);
                if (status != NtStatus::SUCCESS) {
                    return make_error(header, status);
                }
                return ByteBuffer(0);
            }
            return make_error(header, NtStatus::INVALID_PARAMETER);
        }

        auto status = lock_mgr_.ack_oplock_break(ack->file_id, session.session_id(),
                                                 ack->oplock_level);
        if (status != NtStatus::SUCCESS) {
            return make_error(header, status);
        }

        Smb2OplockBreakAckResponse resp_ack;
        resp_ack.oplock_level = ack->oplock_level;
        resp_ack.file_id = ack->file_id;

        ByteBuffer buf(SMB2_HEADER_SIZE + 24);
        auto hdr = Smb2Codec::make_response_header(header, static_cast<uint16_t>(Smb2Command::OPLOCK_BREAK));
        buf.append(Smb2Codec::encode_header(hdr));
        Smb2Codec::write_le16(buf, resp_ack.structure_size);
        buf.append_u8(resp_ack.oplock_level);
        buf.append_u8(resp_ack.reserved);
        Smb2Codec::write_le32(buf, resp_ack.reserved2);
        Smb2Codec::write_le64(buf, resp_ack.file_id.persistent);
        Smb2Codec::write_le64(buf, resp_ack.file_id.volatile_id);
        return buf;
    }

    DialectRevision SmbDispatcher::select_dialect(const std::vector<uint16_t> & client_dialects)
    {
        DialectRevision best = DialectRevision::SMB_2_002;
        for (uint16_t d : client_dialects) {
            DialectRevision dr = static_cast<DialectRevision>(d);
            if (static_cast<uint16_t>(dr) > static_cast<uint16_t>(best)) {
                for (auto supported : config_.supported_dialects) {
                    if (dr == supported) {
                        best = dr;
                        break;
                    }
                }
            }
        }
        for (auto supported : config_.supported_dialects) {
            if (best == supported)
                return best;
        }
        return DialectRevision::SMB_2_002;
    }

    uint32_t SmbDispatcher::compute_server_capabilities(DialectRevision dialect)
    {
        uint32_t caps = 0;
        if (dialect >= DialectRevision::SMB_2_1) {
            caps |= SMB2_GLOBAL_CAP_LEASING | SMB2_GLOBAL_CAP_LARGE_MTU;
        }
        if (dialect >= DialectRevision::SMB_3_0 && config_.enable_encryption) {
            caps |= SMB2_GLOBAL_CAP_ENCRYPTION;
        }
        return caps;
    }

    uint16_t SmbDispatcher::compute_security_mode() const
    {
        uint16_t mode = static_cast<uint16_t>(SecurityMode::NEGOTIATE_SIGNING_ENABLED);
        if (config_.require_signing) {
            mode |= static_cast<uint16_t>(SecurityMode::NEGOTIATE_SIGNING_REQUIRED);
        }
        return mode;
    }

    ByteBuffer SmbDispatcher::make_error(const Smb2Header & header, NtStatus status)
    {
        return Smb2Codec::build_error_response(header, status);
    }

    void SmbDispatcher::derive_session_keys(SmbSession & session, const std::vector<uint8_t> & session_key)
    {
        DialectRevision dialect = session.dialect();

        auto signing_key = SmbKeyDerivation::derive_signing_key(session_key, dialect, session.preauth_hash());
        session.set_signing_key(signing_key);

        if (dialect >= DialectRevision::SMB_3_0 && config_.enable_encryption) {
            auto enc_key = SmbKeyDerivation::derive_encryption_key(session_key, dialect, session.preauth_hash());
            auto dec_key = SmbKeyDerivation::derive_decryption_key(session_key, dialect, session.preauth_hash());
            auto enc_iv = SmbKeyDerivation::derive_encryption_iv(session_key, dialect, session.preauth_hash());
            auto dec_iv = SmbKeyDerivation::derive_decryption_iv(session_key, dialect, session.preauth_hash());

            session.set_encryption_key(enc_key);
            session.set_decryption_key(dec_key);
            session.set_encryption_iv(enc_iv);
            session.set_decryption_iv(dec_iv);
        }
    }
}
