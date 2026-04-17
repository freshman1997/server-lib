#ifndef __NET_SMB_SMB_DISPATCHER_H__
#define __NET_SMB_SMB_DISPATCHER_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include "protocol/smb2_codec.h"
#include "smb_config.h"
#include "smb_handler.h"
#include "smb_session.h"
#include "smb_share.h"
#include "smb_lock_manager.h"
#include "smb_pipe_manager.h"
#include "smb_dfs_resolver.h"
#include "smb_change_notifier.h"
#include "auth/smb_auth.h"
#include "auth/smb_spnego.h"
#include "crypto/smb_crypto.h"
#include "crypto/smb_key_derivation.h"
#include "buffer/byte_buffer.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::net::smb
{
    class SmbDispatcher
    {
    public:
        SmbDispatcher(const SmbServerConfig &config,
                      SmbShareManager &share_mgr,
                      SmbLockManager &lock_mgr,
                      SmbPipeManager &pipe_mgr,
                      SmbDfsResolver &dfs_resolver,
                      SmbChangeNotifier &change_notifier,
                      SmbHandler *handler = nullptr);

        ByteBuffer dispatch(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        std::vector<ByteBuffer> dispatch_compound(SmbSession &session, const uint8_t *data, size_t len);

        void set_crypto(std::shared_ptr<SmbCrypto> crypto)
        {
            crypto_ = crypto;
        }

    private:
        ByteBuffer handle_negotiate(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_session_setup(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_logoff(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_tree_connect(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_tree_disconnect(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_create(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_close(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_read(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_write(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_query_directory(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_query_info(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_set_info(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_lock(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_ioctl(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_echo(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_flush(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_change_notify(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_cancel(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);
        ByteBuffer handle_oplock_break(SmbSession &session, const Smb2Header &header, const uint8_t *data, size_t len);

        DialectRevision select_dialect(const std::vector<uint16_t> &client_dialects);
        uint32_t compute_server_capabilities(DialectRevision dialect);
        uint16_t compute_security_mode() const;

        ByteBuffer make_error(const Smb2Header &header, NtStatus status);
        void derive_session_keys(SmbSession &session, const std::vector<uint8_t> &session_key);

        uint8_t server_guid_[16] = {};

        const SmbServerConfig &config_;
        SmbShareManager &share_mgr_;
        SmbLockManager &lock_mgr_;
        SmbPipeManager &pipe_mgr_;
        SmbDfsResolver &dfs_resolver_;
        SmbChangeNotifier &change_notifier_;
        SmbHandler *handler_;
        std::shared_ptr<SmbCrypto> crypto_;
    };
}
#endif
