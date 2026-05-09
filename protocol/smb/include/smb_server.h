#ifndef __NET_SMB_SMB_SERVER_H__
#define __NET_SMB_SMB_SERVER_H__

#include "smb_config.h"
#include "smb_handler.h"
#include "smb_session.h"
#include "smb_share.h"
#include "smb_lock_manager.h"
#include "smb_pipe_manager.h"
#include "smb_dfs_resolver.h"
#include "smb_change_notifier.h"
#include "smb_dispatcher.h"
#include "smb_file_system.h"
#include "crypto/smb_crypto.h"
#include "crypto/smb_crypto_openssl.h"
#include "protocol/smb_netbios.h"
#include "protocol/smb1_negotiate.h"
#include "protocol/smb2_codec.h"
#include "buffer/byte_buffer.h"
#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace yuan::net::smb
{
    class SmbServer
    {
    public:
        SmbServer();
        explicit SmbServer(const SmbServerConfig &config);
        ~SmbServer();

        SmbServer(const SmbServer &) = delete;
        SmbServer &operator=(const SmbServer &) = delete;

        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        void serve();
        void stop();

        NetworkRuntime *runtime() const noexcept
        {
            return listener_.runtime();
        }

        void set_handler(SmbHandler *handler)
        {
            handler_ = handler;
            dispatcher_.set_handler_ptr(handler);
        }

        const SmbServerConfig &config() const
        {
            return config_;
        }

        SmbShareManager &share_manager()
        {
            return share_mgr_;
        }

        SmbLockManager &lock_manager()
        {
            return lock_mgr_;
        }

        SmbPipeManager &pipe_manager()
        {
            return pipe_mgr_;
        }

        SmbDfsResolver &dfs_resolver()
        {
            return dfs_resolver_;
        }

        SmbChangeNotifier &change_notifier()
        {
            return change_notifier_;
        }

        SmbSessionManager &session_manager()
        {
            return session_mgr_;
        }

    private:
        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);

        ByteBuffer process_message(SmbSession *session, const uint8_t *data, size_t len);
        ByteBuffer handle_smb1_negotiate(const uint8_t *data, size_t len);

        bool try_decrypt(const SmbSession *session, const uint8_t *data, size_t len,
                         std::vector<uint8_t> &out);
        ByteBuffer try_encrypt(const SmbSession *session, ByteBuffer &&resp);
        void try_sign(const SmbSession *session, ByteBuffer &resp);

        void init_shares();
        void init_pipes();

        class DispatcherWrapper
        {
        public:
            DispatcherWrapper(const SmbServerConfig &config,
                              SmbShareManager &share_mgr,
                              SmbLockManager &lock_mgr,
                              SmbPipeManager &pipe_mgr,
                              SmbDfsResolver &dfs_resolver,
                              SmbChangeNotifier &change_notifier)
                : dispatcher_(config, share_mgr, lock_mgr, pipe_mgr, dfs_resolver, change_notifier)
            {
            }

            SmbDispatcher &dispatcher()
            {
                return dispatcher_;
            }

            void set_handler_ptr(SmbHandler *handler)
            {
                handler_ = handler;
                dispatcher_.set_handler(handler);
            }

            SmbHandler *handler_ptr() const
            {
                return handler_;
            }

        private:
            SmbDispatcher dispatcher_;
            SmbHandler *handler_ = nullptr;
        };

        AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        coroutine::Task<void> accept_task_;
        SmbServerConfig config_;
        SmbShareManager share_mgr_;
        SmbLockManager lock_mgr_;
        SmbPipeManager pipe_mgr_;
        SmbDfsResolver dfs_resolver_;
        SmbChangeNotifier change_notifier_;
        SmbSessionManager session_mgr_;
        DispatcherWrapper dispatcher_;
        std::shared_ptr<SmbCrypto> crypto_;
        SmbHandler *handler_ = nullptr;
        std::atomic<bool> running_{ false };
    };
}

#endif
