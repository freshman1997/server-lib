#include "smb_server.h"
#include "smb_file_system.h"
#include "protocol/smb2_constants.h"
#include "crypto/smb_crypto_openssl.h"
#include <random>
#include <cstring>

namespace yuan::net::smb
{
    namespace
    {
        bool command_allows_unsigned(uint16_t command)
        {
            return command == static_cast<uint16_t>(Smb2Command::NEGOTIATE) ||
                   command == static_cast<uint16_t>(Smb2Command::SESSION_SETUP);
        }

        std::vector<uint8_t> copy_with_zero_signature(const uint8_t *data, size_t len)
        {
            std::vector<uint8_t> signed_view(data, data + len);
            if (signed_view.size() >= SMB2_HEADER_SIZE) {
                std::memset(signed_view.data() + 48, 0, SMB2_SIGNATURE_SIZE);
            }
            return signed_view;
        }
    }

    SmbServer::SmbServer()
        : config_(), dispatcher_(config_, share_mgr_, lock_mgr_, pipe_mgr_, dfs_resolver_, change_notifier_)
    {
        crypto_ = std::make_shared<SmbCryptoOpenSSL>();
        dispatcher_.dispatcher().set_crypto(crypto_);
        init_shares();
        init_pipes();
    }

    SmbServer::SmbServer(const SmbServerConfig & config)
        : config_(config), dispatcher_(config_, share_mgr_, lock_mgr_, pipe_mgr_, dfs_resolver_, change_notifier_)
    {
        crypto_ = std::make_shared<SmbCryptoOpenSSL>();
        dispatcher_.dispatcher().set_crypto(crypto_);
        init_shares();
        init_pipes();
    }

    SmbServer::~SmbServer()
    {
        stop();
    }

    bool SmbServer::init(int port)
    {
        config_.port = static_cast<uint16_t>(port);
        owned_runtime_ = std::make_unique<NetworkRuntime>();
        return listener_.bind(port, *owned_runtime_);
    }

    bool SmbServer::init(int port, NetworkRuntime & runtime)
    {
        config_.port = static_cast<uint16_t>(port);
        return listener_.bind(port, runtime);
    }

    void SmbServer::serve()
    {
        running_.store(true);

        listener_.set_connection_handler(
            [this](AsyncConnectionContext ctx)->coroutine::Task<void> {
                co_await handle_connection(std::move(ctx));
            });

        accept_task_ = listener_.run_async();
        accept_task_.resume();

        if (owned_runtime_) {
            owned_runtime_->event_loop()->loop();
        }

        accept_task_ = {};
    }

    void SmbServer::stop()
    {
        running_.store(false);
        listener_.close();
        if (owned_runtime_) {
            owned_runtime_->event_loop()->quit();
        }
    }

    coroutine::Task<void> SmbServer::handle_connection(AsyncConnectionContext ctx)
    {
        SmbSession *session = session_mgr_.create_session(ctx.connection());

        ByteBuffer recv_buf;

        while (running_.load() && ctx.is_connected()) {
            auto read_result = co_await ctx.read_async(config_.idle_timeout_ms);
            if (read_result.status != coroutine::IoStatus::success) {
                break;
            }

            if (read_result.data.readable_bytes() > 0) {
                recv_buf.append(read_result.data);
            }

            auto messages = SmbNetbios::split_messages(recv_buf);
            if (!messages) {
                continue;
            }

            for (auto &msg : *messages) {
                auto span = msg.readable_span();
                const uint8_t *raw = reinterpret_cast<const uint8_t *>(span.data());
                size_t raw_len = span.size();

                if (raw_len <= 4) {
                    continue;
                }

                const uint8_t *data = raw + 4;
                size_t len = raw_len - 4;

                std::vector<uint8_t> decrypted;
                const uint8_t *payload = data;
                size_t payload_len = len;

                if (Smb2Codec::is_transform_header(data, len)) {
                    if (!try_decrypt(session, data, len, decrypted)) {
                        session->close();
                        session_mgr_.remove_session(session->session_id());
                        co_return;
                    }
                    payload = decrypted.data();
                    payload_len = decrypted.size();
                }

                ByteBuffer resp = process_message(session, payload, payload_len);
                if (resp.readable_bytes() == 0) {
                    continue;
                }

                resp = try_encrypt(session, std::move(resp));
                try_sign(session, resp);

                ByteBuffer netbios_frame = SmbNetbios::encode(
                    static_cast<uint32_t>(resp.readable_bytes()));
                netbios_frame.append(resp);

                co_await ctx.write_async(netbios_frame);
            }
        }

        session->close();
        session_mgr_.remove_session(session->session_id());
    }

    ByteBuffer SmbServer::process_message(SmbSession * session, const uint8_t * data, size_t len)
    {
        if (Smb1Negotiate::is_smb1_negotiate(data, len)) {
            return handle_smb1_negotiate(data, len);
        }

        if (len < SMB2_HEADER_SIZE) {
            return {};
        }

        auto header = Smb2Codec::decode_header(data, len);
        if (!header) {
            return {};
        }

        if (session->is_signed() && !command_allows_unsigned(header->command)) {
            if ((header->flags & SMB2_FLAGS_SIGNED) != 0) {
                auto signed_view = copy_with_zero_signature(data, len);
                if (!crypto_->verify(session->signing_key(), signed_view.data(), signed_view.size(), header->signature)) {
                    return Smb2Codec::build_error_response(*header, NtStatus::ACCESS_DENIED);
                }
            } else if (config_.require_signing) {
                return Smb2Codec::build_error_response(*header, NtStatus::ACCESS_DENIED);
            }
        }

        if (header->command == static_cast<uint16_t>(Smb2Command::CANCEL)) {
            return dispatcher_.dispatcher().dispatch(*session, *header,
                                                     data, len);
        }

        if (header->next_command > 0) {
            auto responses = dispatcher_.dispatcher().dispatch_compound(*session, data, len);
            ByteBuffer combined;
            for (auto &r : responses) {
                combined.append(r);
            }
            return combined;
        }

        return dispatcher_.dispatcher().dispatch(*session, *header,
                                                 data, len);
    }

    ByteBuffer SmbServer::handle_smb1_negotiate(const uint8_t * data, size_t len)
    {
        if (!config_.enable_smb1_fallback) {
            return {};
        }

        auto req = Smb1Negotiate::decode(data, len);
        if (!req) {
            return {};
        }

        if (req->supports_smb2) {
            return Smb1Negotiate::build_smb2_negotiate_redirect(config_.server_name);
        }

        return {};
    }

    bool SmbServer::try_decrypt(const SmbSession * session, const uint8_t * data, size_t len,
                                std::vector<uint8_t> & out)
    {
        auto transform = Smb2Codec::decode_transform_header(data, len);
        if (!transform) {
            return false;
        }

        if (!session->is_encrypted()) {
            return false;
        }

        const auto &key = session->decryption_key();

        size_t header_size = 52;
        const uint8_t *encrypted_data = data + header_size;
        size_t encrypted_len = len - header_size;

        const uint8_t *nonce = transform->nonce;
        size_t nonce_len = 16;

        const uint8_t *aad = data;
        size_t aad_len = header_size;

        out.resize(encrypted_len);
        size_t out_len = out.size();

        if (!crypto_->decrypt(key, nonce, nonce_len, aad, aad_len,
                              encrypted_data, encrypted_len, out.data(), out_len)) {
            return false;
        }

        out.resize(out_len);
        return true;
    }

    ByteBuffer SmbServer::try_encrypt(const SmbSession * session, ByteBuffer && resp)
    {
        if (!session->is_encrypted()) {
            return std::move(resp);
        }

        auto span = resp.readable_span();
        const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());
        size_t len = span.size();

        const auto &key = session->encryption_key();

        uint8_t nonce[16] = {};
        std::random_device rd;
        for (int i = 0; i < 16; ++i) {
            nonce[i] = static_cast<uint8_t>(rd() & 0xFF);
        }

        Smb2TransformHeader th;
        std::memset(&th, 0, sizeof(th));
        th.protocol_id = SMB2_TRANSFORM_PROTOCOL_ID;
        th.session_id = session->session_id();
        std::memcpy(th.nonce, nonce, 16);
        th.original_message_size = static_cast<uint32_t>(len);

        if (session->dialect() >= DialectRevision::SMB_3_1_1) {
            th.encryption_algorithm = static_cast<uint16_t>(EncryptionAlgorithm::AES_128_GCM);
        } else {
            th.encryption_algorithm = static_cast<uint16_t>(EncryptionAlgorithm::AES_128_CCM);
        }

        ByteBuffer aad_buf = Smb2Codec::encode_transform_header(th);
        auto aad_span = aad_buf.readable_span();
        const uint8_t *aad = reinterpret_cast<const uint8_t *>(aad_span.data());

        auto encrypted = crypto_->encrypt(key, nonce, 16, aad, aad_buf.readable_bytes(), data, len);

        ByteBuffer frame = Smb2Codec::encode_transform_header(th);
        frame.append(encrypted.data(), encrypted.size());

        return frame;
    }

    void SmbServer::try_sign(const SmbSession * session, ByteBuffer & resp)
    {
        if (!session->is_signed()) {
            return;
        }

        if (resp.readable_bytes() < SMB2_HEADER_SIZE) {
            return;
        }

        if (Smb2Codec::is_transform_header(
                reinterpret_cast<const uint8_t *>(resp.readable_span().data()),
                resp.readable_bytes())) {
            return;
        }

        auto span = resp.readable_span();
        const uint8_t *data = reinterpret_cast<const uint8_t *>(span.data());
        size_t total = resp.readable_bytes();

        std::vector<uint8_t> signed_view(data, data + total);
        uint32_t flags = Smb2Codec::read_le32(signed_view.data() + 16);
        flags |= SMB2_FLAGS_SIGNED;
        signed_view[16] = static_cast<uint8_t>(flags & 0xFF);
        signed_view[17] = static_cast<uint8_t>((flags >> 8) & 0xFF);
        signed_view[18] = static_cast<uint8_t>((flags >> 16) & 0xFF);
        signed_view[19] = static_cast<uint8_t>((flags >> 24) & 0xFF);
        std::memset(signed_view.data() + 48, 0, SMB2_SIGNATURE_SIZE);

        auto signature = crypto_->sign(session->signing_key(), signed_view.data(), signed_view.size());
        if (signature.size() >= 16) {
            ByteBuffer signed_buf(total);
            signed_buf.append(signed_view.data(), 48);
            signed_buf.append(signature.data(), 16);
            signed_buf.append(signed_view.data() + 64, total - 64);
            resp = std::move(signed_buf);
        }
    }

    void SmbServer::init_shares()
    {
        for (const auto &share_cfg : config_.shares) {
            share_mgr_.add_share(share_cfg);
            auto share_owner = share_mgr_.find_share_owner(share_cfg.name);
            auto *share = share_owner ? const_cast<SmbShare *>(&*share_owner) : nullptr;
            if (share && share->type() == ShareType::DISK && !share_cfg.path.empty()) {
                auto fs = std::make_unique<LocalFileSystem>(share_cfg.path);
                share->set_file_system(std::move(fs));
            }
        }

        if (!share_mgr_.find_share_owner("IPC$")) {
            SmbShareConfig ipc;
            ipc.name = "IPC$";
            ipc.type = ShareType::PIPE;
            ipc.comment = "Remote IPC";
            share_mgr_.add_share(ipc);
        }
    }

    void SmbServer::init_pipes()
    {
        pipe_mgr_.register_builtin_pipes();
    }
}
