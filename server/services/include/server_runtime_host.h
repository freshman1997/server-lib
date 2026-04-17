#ifndef __SERVER_SERVER_RUNTIME_HOST_H__
#define __SERVER_SERVER_RUNTIME_HOST_H__

#include "runtime_context.h"
#include "server_service_events.h"

#include <any>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace yuan::server
{

    class ServerRuntimeHost
    {
    public:
        struct Config
        {
            std::string service_name;
            std::string protocol;
            int port = 0;
        };

        explicit ServerRuntimeHost(Config config);
        ~ServerRuntimeHost();

        ServerRuntimeHost(const ServerRuntimeHost &) = delete;
        ServerRuntimeHost &operator=(const ServerRuntimeHost &) = delete;

        void set_runtime_context(const yuan::app::RuntimeContext &context);

        bool start(std::function<void()> serve_fn);
        void stop(std::function<void()> stop_fn = nullptr);

        bool is_started() const;

        void publish_custom(const std::string &event_name, std::any payload = {});

        template <typename T>
        void publish_custom(const std::string &event_name, T &&payload)
        {
            publish_custom(event_name, std::any(std::forward<T>(payload)));
        }

    private:
        ServiceRuntimeEvent make_event() const;
        void publish(const char *event_type);

        Config config_;
        yuan::app::RuntimeContext runtime_context_{};
        std::atomic<bool> started_{ false };
        std::thread worker_;
    };

} // namespace yuan::server

#endif
