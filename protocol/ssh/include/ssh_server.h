#ifndef __NET_SSH_SSH_SERVER_H__
#define __NET_SSH_SSH_SERVER_H__

#include "ssh_config.h"
#include "ssh_handler.h"
#include "ssh_session.h"
#include "algorithm/ssh_algorithm_registry.h"
#include "crypto/ssh_crypto.h"
#include "hostkey/ssh_host_key_provider.h"
#include "sftp/ssh_file_system.h"
#include "buffer/byte_buffer.h"
#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace yuan::net::ssh
{
    class SshServer
    {
    public:
        SshServer();
        explicit SshServer(const SshServerConfig &config);
        ~SshServer();

        SshServer(const SshServer &) = delete;
        SshServer &operator=(const SshServer &) = delete;

        bool init(int port);
        bool init(int port, NetworkRuntime &runtime);
        void serve();
        void stop();

        NetworkRuntime *runtime() const noexcept
        {
            return listener_.runtime();
        }

        void set_handler(SshHandler *handler)
        {
            handler_ = handler;
        }

        SshHandler *handler() const
        {
            return handler_;
        }

        const SshServerConfig &config() const
        {
            return config_;
        }

        SshAlgorithmRegistry &algorithm_registry()
        {
            return algo_registry_;
        }

        SshHostKeyProvider &host_key_provider()
        {
            return host_key_provider_;
        }

        SshSessionManager &session_manager()
        {
            return session_mgr_;
        }

        void register_subsystem(const std::string &name, SshConnectionManager::SubsystemFactory factory);

    private:
        coroutine::Task<void> handle_connection(AsyncConnectionContext ctx);

        void init_default_algorithms();
        void init_auth_methods(SshSession *session);

        AsyncListenerHost listener_;
        std::unique_ptr<NetworkRuntime> owned_runtime_;
        SshServerConfig config_;
        SshAlgorithmRegistry algo_registry_;
        std::unique_ptr<SshCrypto> crypto_;
        SshHostKeyProvider host_key_provider_;
        SshSessionManager session_mgr_;
        SshHandler *handler_ = nullptr;
        std::atomic<bool> running_{ false };

        std::unique_ptr<SshFileSystem> file_system_;
        std::unordered_map<std::string, SshConnectionManager::SubsystemFactory> subsystem_factories_;
    };
}

#endif
