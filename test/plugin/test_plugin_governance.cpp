#include "bootstrap.h"
#include "eventbus/event_bus.h"
#include "plugin_resource_guard.h"
#include "plugin_protocol_service_adapter.h"
#include "plugin_host_service.h"
#include "plugin_service_registry_adapter.h"
#include "plugin/plugin_call_guard.h"
#include "plugin/plugin_context.h"
#include "plugin/plugin_events.h"
#include "plugin/script_plugin_adapter.h"
#include "plugin/script_plugin_registry.h"
#include "plugin/plugin_manager.h"
#include "plugin/plugin_lifecycle_manager.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_meta.h"
#include "plugin/plugin_state.h"
#include "lua_script_plugin_adapter.h"
#include "net/runtime/network_runtime.h"
#include "ts_script_plugin_adapter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

#include "buffer/byte_buffer.h"
#include "nlohmann/json.hpp"

#include <any>

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    std::unordered_map<std::string, int> fake_script_load_counts;
    int fake_script_cleanup_count = 0;

    void close_socket(socket_t fd)
    {
        if (fd == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    uint16_t reserve_tcp_port()
    {
        const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener == kInvalidSocket) {
            return 0;
        }

        int reuse = 1;
#ifdef _WIN32
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
        (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_socket(listener);
            return 0;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            close_socket(listener);
            return 0;
        }

        const uint16_t port = ntohs(bound.sin_port);
        close_socket(listener);
        return port;
    }

    bool tcp_echo_roundtrip(uint16_t port, const std::string &payload)
    {
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocket) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        bool connected = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
                connected = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connected) {
            close_socket(fd);
            return false;
        }

        std::size_t sent = 0;
        while (sent < payload.size()) {
            const auto rc = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
            if (rc <= 0) {
                close_socket(fd);
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }

        std::string received;
        received.resize(payload.size());
        std::size_t used = 0;
        while (used < received.size()) {
            const auto rc = ::recv(fd, received.data() + used, received.size() - used, 0);
            if (rc <= 0) {
                close_socket(fd);
                return false;
            }
            used += static_cast<std::size_t>(rc);
        }

        close_socket(fd);
        return received == payload;
    }

    bool udp_send_and_expect_echo(uint16_t port,
                                  const std::string &payload,
                                  std::chrono::milliseconds timeout)
    {
        const socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == kInvalidSocket) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        const auto sent = ::sendto(
            fd,
            payload.data(),
            static_cast<int>(payload.size()),
            0,
            reinterpret_cast<const sockaddr *>(&addr),
            sizeof(addr));
        if (sent != static_cast<int>(payload.size())) {
            close_socket(fd);
            return false;
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(fd, &read_set);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout.count() / 1000);
        tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
        const int ready = ::select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr, &tv);
        if (ready <= 0 || !FD_ISSET(fd, &read_set)) {
            close_socket(fd);
            return false;
        }

        char buffer[2048];
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const auto received = ::recvfrom(
            fd,
            buffer,
            static_cast<int>(sizeof(buffer)),
            0,
            reinterpret_cast<sockaddr *>(&from),
            &from_len);

        close_socket(fd);
        return received == static_cast<int>(payload.size()) &&
               std::string(buffer, static_cast<std::size_t>(received)) == payload;
    }

    bool udp_send_payload(uint16_t port, const std::string &payload)
    {
        const socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == kInvalidSocket) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        const auto sent = ::sendto(
            fd,
            payload.data(),
            static_cast<int>(payload.size()),
            0,
            reinterpret_cast<const sockaddr *>(&addr),
            sizeof(addr));
        close_socket(fd);
        return sent == static_cast<int>(payload.size());
    }

    socket_t connect_tcp_socket(uint16_t port)
    {
        const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kInvalidSocket) {
            return kInvalidSocket;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0) {
                return fd;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        close_socket(fd);
        return kInvalidSocket;
    }

    bool tcp_send(socket_t fd, const std::string &payload)
    {
        if (fd == kInvalidSocket) {
            return false;
        }

        std::size_t sent = 0;
        while (sent < payload.size()) {
            const auto rc = ::send(fd, payload.data() + sent, payload.size() - sent, 0);
            if (rc <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(rc);
        }

        return true;
    }

    void append_length_prefix(std::string &buffer, uint32_t payload_size)
    {
        buffer.push_back(static_cast<char>((payload_size >> 24u) & 0xffu));
        buffer.push_back(static_cast<char>((payload_size >> 16u) & 0xffu));
        buffer.push_back(static_cast<char>((payload_size >> 8u) & 0xffu));
        buffer.push_back(static_cast<char>(payload_size & 0xffu));
    }

    std::string make_length_prefixed_frame(const std::string &payload)
    {
        std::string frame;
        frame.reserve(4 + payload.size());
        append_length_prefix(frame, static_cast<uint32_t>(payload.size()));
        frame.append(payload);
        return frame;
    }

    bool tcp_receive_exact(socket_t fd,
                           std::size_t expected_bytes,
                           std::chrono::milliseconds timeout,
                           std::string &data)
    {
        data.clear();
        if (fd == kInvalidSocket) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (data.size() < expected_bytes && std::chrono::steady_clock::now() < deadline) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                break;
            }

            timeval tv{};
            tv.tv_sec = static_cast<long>(remaining.count() / 1000);
            tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);

            const int ready = ::select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr, &tv);
            if (ready <= 0 || !FD_ISSET(fd, &read_set)) {
                continue;
            }

            char buffer[256];
            const auto bytes_to_read = (std::min)(expected_bytes - data.size(), sizeof(buffer));
            const auto rc = ::recv(fd, buffer, static_cast<int>(bytes_to_read), 0);
            if (rc <= 0) {
                return false;
            }
            data.append(buffer, static_cast<std::size_t>(rc));
        }

        return data.size() == expected_bytes;
    }

    bool tcp_receive_length_prefixed_frame(socket_t fd,
                                           std::chrono::milliseconds timeout,
                                           std::string &payload)
    {
        std::string header;
        if (!tcp_receive_exact(fd, 4, timeout, header)) {
            return false;
        }

        const auto frame_size =
            (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24u) |
            (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16u) |
            (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8u) |
            static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
        return tcp_receive_exact(fd, frame_size, timeout, payload);
    }

    bool wait_for_socket_close(socket_t fd, std::chrono::milliseconds timeout)
    {
        if (fd == kInvalidSocket) {
            return true;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);

            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;

            const int ready = ::select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr, &tv);
            if (ready < 0) {
                return false;
            }
            if (ready == 0 || !FD_ISSET(fd, &read_set)) {
                continue;
            }

            char ch = 0;
            const auto rc = ::recv(fd, &ch, 1, 0);
            if (rc == 0) {
                return true;
            }
            if (rc < 0) {
                return true;
            }
        }

        return false;
    }

    struct SocketDrainResult
    {
        bool closed = false;
        std::string data;
    };

    SocketDrainResult drain_socket_until_close(socket_t fd, std::chrono::milliseconds timeout)
    {
        SocketDrainResult result;
        if (fd == kInvalidSocket) {
            result.closed = true;
            return result;
        }

        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);

            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;

            const int ready = ::select(static_cast<int>(fd) + 1, &read_set, nullptr, nullptr, &tv);
            if (ready < 0) {
                result.closed = true;
                return result;
            }
            if (ready == 0 || !FD_ISSET(fd, &read_set)) {
                continue;
            }

            char buffer[256];
            const auto rc = ::recv(fd, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (rc > 0) {
                result.data.append(buffer, static_cast<std::size_t>(rc));
                continue;
            }

            result.closed = true;
            return result;
        }

        return result;
    }

    std::filesystem::path resolve_plugin_examples_dir()
    {
#ifdef YUAN_TEST_PLUGIN_EXAMPLES_DIR
        const std::filesystem::path configured = YUAN_TEST_PLUGIN_EXAMPLES_DIR;
        if (std::filesystem::exists(configured / "LineEchoProtocol.plugin")) {
            return configured;
        }
#endif

        const auto cwd = std::filesystem::current_path();
        const std::vector<std::filesystem::path> candidates{
            cwd / "plugins" / "examples",
            cwd / "build" / "plugins" / "examples",
            cwd / ".." / ".." / "plugins" / "examples",
            cwd / ".." / ".." / ".." / "plugins" / "examples",
        };

        for (const auto &candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate / "LineEchoProtocol.plugin", ec) && !ec) {
                return std::filesystem::canonical(candidate, ec);
            }
        }

        return {};
    }

    void write_script_protocol_plugin(const std::filesystem::path &root,
                                      const std::string &plugin_name,
                                      const nlohmann::json &protocol_services)
    {
        const auto plugin_dir = root / plugin_name;
        std::filesystem::create_directories(plugin_dir);

        std::ofstream manifest(plugin_dir / "plugin.json");
        require(static_cast<bool>(manifest), "protocol plugin manifest should be creatable");
        auto declared_services = protocol_services;
        if (!declared_services.is_array()) {
            declared_services = nlohmann::json::array({ declared_services });
        }
        const auto doc = nlohmann::json{
            {"run_mode", "script"},
            {"language", "lua"},
            {"entry", "main.lua"},
            {"permissions", "register_protocol_service,listen_tcp,listen_udp,bind_privileged_port,use_logger,use_network_runtime"},
            {"protocol_services", declared_services},
        };
        manifest << doc.dump(2) << "\n";
        manifest.close();

        std::ofstream script(plugin_dir / "main.lua");
        require(static_cast<bool>(script), "protocol plugin script should be creatable");
        script << "local plugin = {}\n";
        script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
        script << "function plugin.on_enable() end\n";
        script << "function plugin.on_disable() end\n";
        script << "function plugin.on_health_check() return true end\n";
        script << "function plugin.on_release() end\n";
        script << "return plugin\n";
    }

    class FakeEventBus : public yuan::plugin::HostEventBus
    {
    public:
        yuan::plugin::HostEventSubscription subscribe(const std::string &, yuan::plugin::HostEventHandler) override
        {
            return 1;
        }
        bool unsubscribe(yuan::plugin::HostEventSubscription) override
        {
            return true;
        }
        void publish(std::string, std::any) override
        {
        }
    };

    class FakeScheduler : public yuan::plugin::HostScheduler
    {
    public:
        yuan::plugin::HostSchedulerTaskId schedule_after(std::chrono::milliseconds,
                                                         yuan::plugin::HostSchedulerCallback,
                                                         const std::string &) override
        {
            return 0;
        }
        yuan::plugin::HostSchedulerTaskId schedule_interval(std::chrono::milliseconds,
                                                            yuan::plugin::HostSchedulerCallback,
                                                            const std::string &) override
        {
            return 0;
        }
        bool cancel(yuan::plugin::HostSchedulerTaskId) override
        {
            return true;
        }
        void cancel_by_prefix(const std::string &) override
        {
        }
        bool is_running() const override
        {
            return true;
        }
    };

    class StubPlugin : public yuan::plugin::Plugin
    {
    public:
        void on_loaded() override
        {
        }
        bool on_init(const yuan::plugin::PluginContext &) override
        {
            return true;
        }
        void on_release() override
        {
        }
    };

    class FakeScriptPlugin final : public yuan::plugin::ScriptPluginAdapter
    {
    public:
        using yuan::plugin::ScriptPluginAdapter::ScriptPluginAdapter;

        bool load_script(const std::string &) override
        {
            ++fake_script_load_counts[manifest_.plugin_id];
            script_loaded_ = true;
            return true;
        }

    protected:
        bool do_init(const yuan::plugin::PluginContext &context) override
        {
            if (context.resource_guard) {
                context.resource_guard->track(manifest_.plugin_id,
                                              yuan::plugin::PluginResourceType::callback,
                                              []() { ++fake_script_cleanup_count; },
                                              "fake script reload cleanup");
            }
            return true;
        }
    };

    class TestServiceRegistry final : public yuan::plugin::HostServiceRegistry
    {
    public:
        bool register_service(const std::string &plugin_name,
                              const yuan::plugin::PluginServiceDescriptor &descriptor,
                              std::any service) override
        {
            auto stored = descriptor;
            stored.plugin_name = plugin_name;
            services_[stored.name] = Entry{ stored, std::move(service) };
            return true;
        }

        void unregister_plugin_services(const std::string &plugin_name) override
        {
            for (auto it = services_.begin(); it != services_.end();) {
                if (it->second.descriptor.plugin_name == plugin_name) {
                    it = services_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::any find_service(const std::string &name) const override
        {
            auto it = services_.find(name);
            return it == services_.end() ? std::any{} : it->second.service;
        }

        bool describe_service(const std::string &name, yuan::plugin::PluginServiceDescriptor &descriptor) const override
        {
            auto it = services_.find(name);
            if (it == services_.end()) {
                return false;
            }
            descriptor = it->second.descriptor;
            return true;
        }

        std::vector<yuan::plugin::PluginServiceDescriptor> list_services() const override
        {
            std::vector<yuan::plugin::PluginServiceDescriptor> result;
            for (const auto &pair : services_) {
                result.push_back(pair.second.descriptor);
            }
            return result;
        }

        bool has_service(const std::string &name) const override
        {
            return services_.find(name) != services_.end();
        }

    private:
        struct Entry
        {
            yuan::plugin::PluginServiceDescriptor descriptor;
            std::any service;
        };

        std::unordered_map<std::string, Entry> services_;
    };

    class TestHttpInterceptor final : public yuan::plugin::HostHttpInterceptor
    {
    public:
        yuan::plugin::HttpInterceptorId add_middleware(
            const std::string &plugin_name,
            yuan::plugin::HttpMiddlewareCallback callback,
            const std::string &name = "") override
        {
            const auto id = next_id_++;
            entries_[id] = Entry{ plugin_name, std::move(callback), {}, name, {} };
            return id;
        }

        yuan::plugin::HttpInterceptorId add_route(
            const std::string &plugin_name,
            const std::string &path,
            yuan::plugin::HttpRouteCallback callback,
            const std::string &method = "") override
        {
            const auto id = next_id_++;
            entries_[id] = Entry{ plugin_name, {}, std::move(callback), path, method };
            return id;
        }

        bool remove(yuan::plugin::HttpInterceptorId id) override
        {
            return entries_.erase(id) != 0;
        }

        void remove_by_plugin(const std::string &plugin_name) override
        {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->second.plugin_name == plugin_name) {
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        bool is_available() const override
        {
            return available;
        }

        std::size_t count() const
        {
            return entries_.size();
        }

        bool available = true;

    private:
        struct Entry
        {
            std::string plugin_name;
            yuan::plugin::HttpMiddlewareCallback middleware;
            yuan::plugin::HttpRouteCallback route;
            std::string name_or_path;
            std::string method;
        };

        yuan::plugin::HttpInterceptorId next_id_ = 1;
        std::unordered_map<yuan::plugin::HttpInterceptorId, Entry> entries_;
    };

    void test_state_machine_transitions()
    {
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::discovered, yuan::plugin::PluginState::loaded),
                "discovered -> loaded should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::loaded, yuan::plugin::PluginState::initialized),
                "loaded -> initialized should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::initialized, yuan::plugin::PluginState::active),
                "initialized -> active should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::degraded),
                "active -> degraded should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::faulted),
                "active -> faulted should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::stopping),
                "active -> stopping should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::faulted, yuan::plugin::PluginState::quarantined),
                "faulted -> quarantined should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::faulted, yuan::plugin::PluginState::degraded),
                "faulted -> degraded should be valid (recovery)");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::degraded, yuan::plugin::PluginState::active),
                "degraded -> active should be valid (recovery)");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::stopping, yuan::plugin::PluginState::stopped),
                "stopping -> stopped should be valid");
        require(yuan::plugin::can_transition(yuan::plugin::PluginState::stopped, yuan::plugin::PluginState::unloaded),
                "stopped -> unloaded should be valid");

        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::unloaded, yuan::plugin::PluginState::active),
                "unloaded -> active should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::discovered, yuan::plugin::PluginState::active),
                "discovered -> active should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::active, yuan::plugin::PluginState::discovered),
                "active -> discovered should be invalid");
        require(!yuan::plugin::can_transition(yuan::plugin::PluginState::quarantined, yuan::plugin::PluginState::active),
                "quarantined -> active should be invalid");
    }

    void test_operational_and_callback_states()
    {
        require(yuan::plugin::is_operational(yuan::plugin::PluginState::active), "active should be operational");
        require(yuan::plugin::is_operational(yuan::plugin::PluginState::degraded), "degraded should be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::faulted), "faulted should not be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::quarantined), "quarantined should not be operational");
        require(!yuan::plugin::is_operational(yuan::plugin::PluginState::stopped), "stopped should not be operational");

        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::active), "active should accept callbacks");
        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::degraded), "degraded should accept callbacks");
        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::initialized), "initialized should accept callbacks");
        require(yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::loaded), "loaded should accept init callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::faulted), "faulted should not accept callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::quarantined), "quarantined should not accept callbacks");
        require(!yuan::plugin::accepts_callbacks(yuan::plugin::PluginState::stopped), "stopped should not accept callbacks");
    }

    void test_call_guard_fault_accumulation()
    {
        yuan::plugin::PluginCallGuard guard;

        require(guard.fault_count("test_plugin") == 0, "initial fault count should be 0");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::active,
                "suggested state with 0 faults should be active");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call",
                                []() { throw std::runtime_error("boom"); });
        require(guard.fault_count("test_plugin") == 1, "fault count should be 1 after first exception");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::degraded,
                "suggested state with 1 fault should be degraded");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call2",
                                []() { throw std::runtime_error("boom2"); });
        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call3",
                                []() { throw std::runtime_error("boom3"); });
        require(guard.fault_count("test_plugin") == 3, "fault count should be 3 after 3 exceptions");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::faulted,
                "suggested state with 3 faults should be faulted");

        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call4",
                                []() { throw std::runtime_error("boom4"); });
        guard.guarded_call_void("test_plugin", yuan::plugin::PluginState::active, "test_call5",
                                []() { throw std::runtime_error("boom5"); });
        require(guard.fault_count("test_plugin") == 5, "fault count should be 5 after 5 exceptions");
        require(guard.suggested_state("test_plugin") == yuan::plugin::PluginState::quarantined,
                "suggested state with 5 faults should be quarantined");

        guard.reset_faults("test_plugin");
        require(guard.fault_count("test_plugin") == 0, "fault count should be 0 after reset");
    }

    void test_call_guard_blocks_faulted_plugin()
    {
        yuan::plugin::PluginCallGuard guard;

        bool called = false;
        bool result = guard.guarded_call_void("faulted_plugin", yuan::plugin::PluginState::faulted, "blocked_call",
                                              [&called]() { called = true; });
        require(!result, "guarded_call_void should return false for faulted plugin");
        require(!called, "callback should not execute for faulted plugin");

        called = false;
        result = guard.guarded_call_void("quarantined_plugin", yuan::plugin::PluginState::quarantined, "blocked_call",
                                         [&called]() { called = true; });
        require(!result, "guarded_call_void should return false for quarantined plugin");
        require(!called, "callback should not execute for quarantined plugin");

        called = false;
        result = guard.guarded_call_void("active_plugin", yuan::plugin::PluginState::active, "allowed_call",
                                         [&called]() { called = true; });
        require(result, "guarded_call_void should return true for active plugin");
        require(called, "callback should execute for active plugin");
    }

    void test_call_guard_fault_handler()
    {
        yuan::plugin::PluginCallGuard guard;

        yuan::plugin::FaultEvent last_event;
        int handler_calls = 0;

        guard.set_fault_handler([&](const yuan::plugin::FaultEvent &event) {
        last_event = event;
        ++handler_calls;
        });

        guard.guarded_call_void("handler_test", yuan::plugin::PluginState::active, "site1",
                                []() { throw std::runtime_error("err"); });

        require(handler_calls == 1, "fault handler should be called once");
        require(last_event.plugin_name == "handler_test", "fault event should contain plugin name");
        require(last_event.call_site == "site1", "fault event should contain call site");
        require(last_event.error_message == "err", "fault event should contain error message");
    }

    void test_call_guard_custom_thresholds()
    {
        yuan::plugin::PluginCallGuard::Config config;
        config.fault_threshold = 2;
        config.quarantine_threshold = 4;

        yuan::plugin::PluginCallGuard guard(config);

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::degraded,
                "1 fault with threshold=2 should suggest degraded");

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::faulted,
                "2 faults with threshold=2 should suggest faulted");

        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        guard.guarded_call_void("custom", yuan::plugin::PluginState::active, "call",
                                []() { throw std::runtime_error("e"); });
        require(guard.suggested_state("custom") == yuan::plugin::PluginState::quarantined,
                "4 faults with threshold=4 should suggest quarantined");
    }

    void test_lifecycle_manager_state_transitions()
    {
        yuan::plugin::PluginLifecycleManager mgr;

        auto plugin = new StubPlugin();
        require(mgr.register_instance("stub", plugin, nullptr),
                "register_instance should succeed for new plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::loaded,
                "newly registered plugin should be in loaded state");

        require(mgr.transition("stub", yuan::plugin::PluginState::initialized),
                "loaded -> initialized should succeed");
        require(mgr.state("stub") == yuan::plugin::PluginState::initialized,
                "plugin should be in initialized state");

        require(mgr.transition("stub", yuan::plugin::PluginState::active),
                "initialized -> active should succeed");
        require(mgr.state("stub") == yuan::plugin::PluginState::active,
                "plugin should be in active state");

        require(mgr.accepts_callbacks("stub"), "active plugin should accept callbacks");

        require(mgr.fault("stub", "test fault"), "fault should succeed on operational plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::degraded,
                "plugin with 1 fault should be degraded");
        require(mgr.accepts_callbacks("stub"), "degraded plugin should still accept callbacks");

        require(mgr.recover("stub"), "recovery should succeed from degraded");
        require(mgr.state("stub") == yuan::plugin::PluginState::active,
                "recovered plugin should be active");
        require(mgr.call_guard().fault_count("stub") == 0,
                "fault count should be reset after recovery");

        require(mgr.stop("stub"), "stop should succeed for active plugin");
        require(mgr.state("stub") == yuan::plugin::PluginState::stopped,
                "stopped plugin should be in stopped state");
        require(!mgr.accepts_callbacks("stub"), "stopped plugin should not accept callbacks");

        mgr.unload("stub");
    }

    void test_lifecycle_manager_stop_from_loaded_state()
    {
        yuan::plugin::PluginLifecycleManager mgr;

        auto plugin = new StubPlugin();
        require(mgr.register_instance("loaded_only", plugin, nullptr),
                "register_instance should succeed for loaded-only plugin");

        require(mgr.stop("loaded_only"),
                "stop should succeed for a plugin that never reached init");
        require(mgr.state("loaded_only") == yuan::plugin::PluginState::stopped,
                "loaded-only plugin should move to stopped state");

        require(mgr.unload("loaded_only"),
                "unload should succeed after stopping loaded-only plugin");
    }

    void test_lifecycle_manager_fault_escalation()
    {
        yuan::plugin::PluginLifecycleManager::Config config;
        config.call_guard_config.fault_threshold = 2;
        config.call_guard_config.quarantine_threshold = 4;

        yuan::plugin::PluginLifecycleManager mgr(config);

        auto plugin = new StubPlugin();
        mgr.register_instance("escalation", plugin, nullptr);
        mgr.transition("escalation", yuan::plugin::PluginState::initialized);
        mgr.transition("escalation", yuan::plugin::PluginState::active);

        mgr.fault("escalation", "fault1");
        require(mgr.state("escalation") == yuan::plugin::PluginState::degraded,
                "1 fault should cause degraded");

        mgr.fault("escalation", "fault2");
        require(mgr.state("escalation") == yuan::plugin::PluginState::faulted,
                "2 faults should cause faulted");
        require(!mgr.accepts_callbacks("escalation"), "faulted plugin should not accept callbacks");

        mgr.recover("escalation");
        require(mgr.state("escalation") == yuan::plugin::PluginState::degraded,
                "recovery from faulted should go to degraded");

        mgr.fault("escalation", "fault3");
        mgr.fault("escalation", "fault4");
        require(mgr.state("escalation") == yuan::plugin::PluginState::quarantined,
                "4 faults should cause quarantined");
        require(!mgr.accepts_callbacks("escalation"), "quarantined plugin should not accept callbacks");

        require(!mgr.recover("escalation"), "recovery should fail for quarantined plugin");
        require(mgr.state("escalation") == yuan::plugin::PluginState::quarantined,
                "quarantined plugin should stay quarantined after failed recovery");

        mgr.stop("escalation");
        mgr.unload("escalation");
    }

    void test_lifecycle_manager_state_change_callback()
    {
        yuan::plugin::PluginLifecycleManager mgr;

        std::vector<std::pair<yuan::plugin::PluginState, yuan::plugin::PluginState> > transitions;

        mgr.set_state_change_callback([&](const std::string &name,
                                          yuan::plugin::PluginState old_state,
                                          yuan::plugin::PluginState new_state) {
        require(name == "cb_test", "callback should receive correct plugin name");
        transitions.push_back({old_state, new_state});
        });

        auto plugin = new StubPlugin();
        mgr.register_instance("cb_test", plugin, nullptr);
        mgr.transition("cb_test", yuan::plugin::PluginState::initialized);
        mgr.transition("cb_test", yuan::plugin::PluginState::active);

        require(transitions.size() == 2, "should have recorded 2 transitions");
        require(transitions[0].first == yuan::plugin::PluginState::loaded &&
                    transitions[0].second == yuan::plugin::PluginState::initialized,
                "first transition should be loaded -> initialized");
        require(transitions[1].first == yuan::plugin::PluginState::initialized &&
                    transitions[1].second == yuan::plugin::PluginState::active,
                "second transition should be initialized -> active");

        mgr.stop("cb_test");
        mgr.unload("cb_test");
    }

    void test_manifest_from_meta()
    {
        yuan::plugin::PluginMeta meta;
        meta.name = "test_plugin";
        meta.version = "2.0.0";
        meta.author = "test_author";
        meta.description = "test desc";
        meta.api_version = 3;
        meta.required_permissions = yuan::plugin::PluginPermission::use_event_bus | yuan::plugin::PluginPermission::use_logger;
        meta.depends_on = { "dep1", "dep2" };

        auto manifest = meta.to_manifest();

        require(manifest.plugin_id == "test_plugin", "manifest plugin_id should match meta name");
        require(manifest.name == "test_plugin", "manifest name should match meta name");
        require(manifest.version == "2.0.0", "manifest version should match meta version");
        require(manifest.author == "test_author", "manifest author should match meta author");
        require(manifest.description == "test desc", "manifest description should match meta description");
        require(manifest.api_version == 3, "manifest api_version should match meta api_version");
        require(yuan::plugin::has_permission(manifest.required_permissions, yuan::plugin::PluginPermission::use_event_bus),
                "manifest should preserve use_event_bus permission");
        require(yuan::plugin::has_permission(manifest.required_permissions, yuan::plugin::PluginPermission::use_logger),
                "manifest should preserve use_logger permission");
        require(manifest.depends_on.size() == 2, "manifest depends_on should be preserved");
        require(manifest.run_mode == yuan::plugin::PluginRunMode::unknown,
                "manifest run_mode should default to unknown");
        require(manifest.extension_points.empty(), "manifest extension_points should default to empty");
    }

    void test_event_descriptors()
    {
        require(std::string(yuan::plugin::event_descriptors::discovered.name) == "plugin.discovered",
                "discovered event name should match");
        require(std::string(yuan::plugin::event_descriptors::discovered.category) == "lifecycle",
                "discovered event category should be lifecycle");
        require(yuan::plugin::event_descriptors::discovered.scope == yuan::plugin::EventScope::host_internal,
                "discovered event scope should be host_internal");
        require(yuan::plugin::event_descriptors::discovered.delivery_semantics == yuan::plugin::EventDeliverySemantics::sync,
                "discovered event should be sync");

        require(std::string(yuan::plugin::event_descriptors::faulted.name) == "plugin.faulted",
                "faulted event name should match");
        require(yuan::plugin::event_descriptors::faulted.delivery_semantics == yuan::plugin::EventDeliverySemantics::sync,
                "faulted event should be sync delivery");

        require(yuan::plugin::event_descriptors::service_registered.required_permission == yuan::plugin::PluginPermission::use_service_registry,
                "service_registered event should require use_service_registry permission");

        require(yuan::plugin::event_descriptors::config_changed.scope == yuan::plugin::EventScope::plugin_local,
                "config_changed event should be plugin_local scope");

        require(std::string(yuan::plugin::event_descriptors::protocol_service_started.name) ==
                    yuan::plugin::events::plugin_protocol_service_started,
                "protocol_service_started event name should match");
        require(yuan::plugin::event_descriptors::protocol_service_started.required_permission ==
                    yuan::plugin::PluginPermission::register_protocol_service,
                "protocol_service_started should require register_protocol_service permission");
        require(std::string(yuan::plugin::event_descriptors::protocol_service_bind_failed.name) ==
                    yuan::plugin::events::plugin_protocol_service_bind_failed,
                "protocol_service_bind_failed event name should match");
        require(yuan::plugin::event_descriptors::protocol_service_bind_failed.delivery_semantics ==
                    yuan::plugin::EventDeliverySemantics::sync,
                "protocol_service_bind_failed should be sync delivery");

        require(std::string(yuan::plugin::event_descriptors::protocol_connection_accepted.name) ==
                    yuan::plugin::events::plugin_protocol_connection_accepted,
                "protocol_connection_accepted event name should match");
        require(std::string(yuan::plugin::event_descriptors::protocol_connection_closed.name) ==
                    yuan::plugin::events::plugin_protocol_connection_closed,
                "protocol_connection_closed event name should match");
        require(std::string(yuan::plugin::event_descriptors::protocol_connection_faulted.name) ==
                    yuan::plugin::events::plugin_protocol_connection_faulted,
                "protocol_connection_faulted event name should match");
        require(yuan::plugin::event_descriptors::protocol_connection_faulted.delivery_semantics ==
                    yuan::plugin::EventDeliverySemantics::sync,
                "protocol_connection_faulted should be sync delivery");
    }

    void test_protocol_service_lifecycle_events()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lifecycle-events-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lifecycle";
        std::filesystem::create_directories(plugin_dir);

        const uint16_t good_port = reserve_tcp_port();
        const uint16_t bad_port = reserve_tcp_port();
        require(good_port != 0 && bad_port != 0,
                "protocol lifecycle event test should reserve TCP ports");

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "protocol lifecycle event manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "line_proto"},
                        {"type", "custom"},
                        {"transport", "tcp"},
                        {"framing", "line"},
                        {"host", "127.0.0.1"},
                        {"port", good_port},
                        {"handler", "main.on_connection"},
                        {"contract_id", "plugin.lifecycle.line"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "protocol lifecycle event script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_connection(conn, data)\n";
            script << "  conn:write(data)\n";
            script << "  conn:write('\\n')\n";
            script << "  conn:flush()\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lifecycle" });
        require(protocol_services.size() == 1,
                "protocol lifecycle event service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lifecycle-event-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.worker_index = 3;
        context.shared_runtime = &runtime;
        context.active_service_name = "proto.lifecycle.service";
        context.service_instance_index = 0;
        context.service_instance_count = 1;
        auto event_bus = std::make_shared<yuan::eventbus::EventBus>();
        context.event_bus = event_bus;

        std::mutex event_mutex;
        std::vector<yuan::plugin::PluginProtocolServiceEvent> started_events;
        std::vector<yuan::plugin::PluginProtocolServiceEvent> stopped_events;
        std::vector<yuan::plugin::PluginProtocolServiceEvent> bind_failed_events;

        event_bus->subscribe(yuan::plugin::events::plugin_protocol_service_started,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolServiceEvent>(&event.payload);
                                 if (!payload) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 started_events.push_back(*payload);
                             });
        event_bus->subscribe(yuan::plugin::events::plugin_protocol_service_stopped,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolServiceEvent>(&event.payload);
                                 if (!payload) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 stopped_events.push_back(*payload);
                             });
        event_bus->subscribe(yuan::plugin::events::plugin_protocol_service_bind_failed,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolServiceEvent>(&event.payload);
                                 if (!payload) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 bind_failed_events.push_back(*payload);
                             });

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        {
            auto service = protocol_services[0];
            service.host = "127.0.0.1";
            service.port = good_port;

            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "protocol lifecycle started/stopped adapter should initialize");
            adapter.start();
            require(adapter.started(), "protocol lifecycle started/stopped adapter should start");
            adapter.stop();
        }

        {
            socket_t blocker = ::socket(AF_INET, SOCK_STREAM, 0);
            require(blocker != kInvalidSocket, "protocol lifecycle bind-failed blocker socket should be creatable");
            int reuse = 1;
#ifdef _WIN32
            (void)::setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#else
            (void)::setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(bad_port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(::bind(blocker, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0,
                    "protocol lifecycle bind-failed blocker should bind reserved port");
            require(::listen(blocker, 1) == 0,
                    "protocol lifecycle bind-failed blocker should listen on reserved port");

            auto service = protocol_services[0];
            service.host = "127.0.0.1";
            service.port = bad_port;

            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "protocol lifecycle bind-failed adapter should initialize");
            adapter.start();
            require(!adapter.started(), "protocol lifecycle bind-failed adapter should not start");
            adapter.stop();
            close_socket(blocker);
        }

        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(event_mutex);
            require(!started_events.empty(),
                    "protocol lifecycle should publish started event");
            require(!stopped_events.empty(),
                    "protocol lifecycle should publish stopped event");
            require(bind_failed_events.size() == 1,
                    "protocol lifecycle should publish bind_failed event");

            const auto &started = started_events.back();
            require(started.plugin_name == "proto_lifecycle",
                    "started event should carry plugin name");
            require(started.protocol_service_name == "line_proto",
                    "started event should carry protocol service name");
            require(started.transport == "tcp" && started.host == "127.0.0.1" && started.port == good_port,
                    "started event should carry endpoint fields");
            require(started.active_service_name == "proto.lifecycle.service",
                    "started event should carry runtime active_service_name");
            require(started.worker_index == 3 && started.service_instance_index == 0,
                    "started event should carry runtime worker/service_instance identity");

            const auto &stopped = stopped_events.back();
            require(stopped.plugin_name == "proto_lifecycle" &&
                        stopped.protocol_service_name == "line_proto",
                    "stopped event should carry plugin/service identity");
            require(stopped.worker_index == 3 && stopped.service_instance_index == 0,
                    "stopped event should carry runtime worker/service_instance identity");

            const auto &bind_failed = bind_failed_events.back();
            require(bind_failed.plugin_name == "proto_lifecycle" &&
                        bind_failed.protocol_service_name == "line_proto",
                    "bind_failed event should carry plugin/service identity");
            require(bind_failed.host == "127.0.0.1" && bind_failed.port == bad_port,
                    "bind_failed event should carry failed endpoint");
            require(bind_failed.worker_index == 3 && bind_failed.service_instance_index == 0,
                    "bind_failed event should carry runtime worker/service_instance identity");
            require(!bind_failed.reason.empty(),
                    "bind_failed event should carry reason");
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_connection_events()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "protocol connection event test requires C++ plugin examples");

        const uint16_t ok_port = reserve_tcp_port();
        const uint16_t fault_port = reserve_tcp_port();
        const uint16_t udp_fault_port = reserve_tcp_port();
        require(ok_port != 0 && fault_port != 0 && udp_fault_port != 0,
                "protocol connection event test should reserve ports");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-connection-events-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        const std::string udp_plugin_name = "proto_udp_connection_events";
        write_script_protocol_plugin(
            temp_root,
            udp_plugin_name,
            nlohmann::json{
                {"name", "udp_connection_events"},
                {"type", "echo"},
                {"transport", "udp"},
                {"host", "127.0.0.1"},
                {"port", udp_fault_port},
                {"framing", "raw"},
                {"max_frame_bytes", 1},
                {"idle_timeout_ms", 200},
                {"contract_id", "plugin.udp.connection.events"},
            });

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        yuan::plugin::PluginManager udp_discovery_manager;
        udp_discovery_manager.set_plugin_path(temp_root.string());
        auto udp_protocol_services = udp_discovery_manager.discover_protocol_services({ udp_plugin_name });
        require(udp_protocol_services.size() == 1,
                "udp connection event manifest should declare one protocol service");
        require(udp_protocol_services[0].plugin_id == udp_plugin_name,
                "udp connection event manifest should preserve plugin identity");
        require(udp_protocol_services[0].max_frame_bytes == 1,
                "udp connection event manifest should preserve max_frame_bytes for fault path");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-connection-event-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.worker_index = 5;
        context.shared_runtime = &runtime;
        context.active_service_name = "proto.connection.events";
        context.service_instance_index = 2;
        context.service_instance_count = 1;

        auto event_bus = std::make_shared<yuan::eventbus::EventBus>();
        context.event_bus = event_bus;

        std::mutex event_mutex;
        std::vector<yuan::plugin::PluginProtocolConnectionEvent> accepted;
        std::vector<yuan::plugin::PluginProtocolConnectionEvent> closed;
        std::vector<yuan::plugin::PluginProtocolConnectionEvent> faulted;
        std::vector<std::pair<std::string, yuan::plugin::PluginProtocolConnectionEvent>> timeline;
        yuan::app::ProtocolServiceRuntimeStatsSnapshot tcp_ok_stats;
        yuan::app::ProtocolServiceRuntimeStatsSnapshot tcp_fault_stats;
        yuan::app::ProtocolServiceRuntimeStatsSnapshot udp_fault_stats;

        const auto tracked_plugin = [&](const std::string &plugin_name) {
            return plugin_name == "LineEchoProtocol" || plugin_name == udp_plugin_name;
        };

        event_bus->subscribe(yuan::plugin::events::plugin_protocol_connection_accepted,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolConnectionEvent>(&event.payload);
                                 if (!payload || !tracked_plugin(payload->plugin_name)) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 accepted.push_back(*payload);
                                 timeline.emplace_back("accepted", *payload);
                             });
        event_bus->subscribe(yuan::plugin::events::plugin_protocol_connection_closed,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolConnectionEvent>(&event.payload);
                                 if (!payload || !tracked_plugin(payload->plugin_name)) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 closed.push_back(*payload);
                                 timeline.emplace_back("closed", *payload);
                             });
        event_bus->subscribe(yuan::plugin::events::plugin_protocol_connection_faulted,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolConnectionEvent>(&event.payload);
                                 if (!payload || !tracked_plugin(payload->plugin_name)) {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(event_mutex);
                                 faulted.push_back(*payload);
                                 timeline.emplace_back("faulted", *payload);
                             });

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = ok_port;
            protocol_service.handler = "line_echo.on_connection";

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "protocol connection event normal adapter should initialize");
            adapter.start();
            require(adapter.started(), "protocol connection event normal adapter should start");
            require(tcp_echo_roundtrip(ok_port, "connection-event-ok\n"),
                    "protocol connection event normal path should roundtrip");
            adapter.stop();

            const auto stats_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < stats_deadline) {
                tcp_ok_stats = adapter.runtime_stats();
                if (tcp_ok_stats.accepted_connection_count > 0 &&
                    tcp_ok_stats.closed_connection_count > 0 &&
                    tcp_ok_stats.active_connection_count == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = fault_port;
            protocol_service.handler = "line_echo.throw_on_data";

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "protocol connection event fault adapter should initialize");
            adapter.start();
            require(adapter.started(), "protocol connection event fault adapter should start");

            const socket_t client = connect_tcp_socket(fault_port);
            require(client != kInvalidSocket, "protocol connection event fault client should connect");
            require(tcp_send(client, "connection-event-fault\n"),
                    "protocol connection event fault path should send payload");
            (void)wait_for_socket_close(client, std::chrono::seconds(2));

            const auto tcp_event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < tcp_event_deadline) {
                bool ready = false;
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    bool seen_faulted = false;
                    bool seen_closed = false;
                    for (const auto &event : faulted) {
                        if (event.plugin_name == "LineEchoProtocol" && event.transport == "tcp") {
                            seen_faulted = true;
                            break;
                        }
                    }
                    for (const auto &event : closed) {
                        if (event.plugin_name == "LineEchoProtocol" && event.transport == "tcp") {
                            seen_closed = true;
                            break;
                        }
                    }
                    ready = seen_faulted && seen_closed;
                }
                if (ready) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            close_socket(client);

            adapter.stop();

            const auto stats_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < stats_deadline) {
                tcp_fault_stats = adapter.runtime_stats();
                if (tcp_fault_stats.accepted_connection_count > 0 &&
                    tcp_fault_stats.closed_connection_count > 0 &&
                    tcp_fault_stats.active_connection_count == 0 &&
                    tcp_fault_stats.handler_error_count > 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        {
            auto protocol_service = udp_protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = udp_fault_port;
            protocol_service.max_frame_bytes = 1;

            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "protocol connection event udp adapter should initialize");
            adapter.start();
            require(adapter.started(), "protocol connection event udp adapter should start");

            const auto udp_warmup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < udp_warmup_deadline) {
                (void)udp_send_payload(udp_fault_port, "udp-ok");
                bool seen_udp_accepted = false;
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    for (const auto &event : accepted) {
                        if (event.plugin_name == udp_plugin_name && event.transport == "udp") {
                            seen_udp_accepted = true;
                            break;
                        }
                    }
                }
                if (seen_udp_accepted) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            const auto udp_event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            bool sent_oversized = false;
            while (std::chrono::steady_clock::now() < udp_event_deadline) {
                sent_oversized = udp_send_payload(udp_fault_port, std::string(2, 'x')) || sent_oversized;

                bool ready = false;
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    bool udp_seen_faulted = false;
                    bool udp_seen_closed = false;
                    for (const auto &event : faulted) {
                        if (event.plugin_name == udp_plugin_name &&
                            event.transport == "udp" &&
                            event.reason.find("udp frame too large") != std::string::npos) {
                            udp_seen_faulted = true;
                            break;
                        }
                    }
                    for (const auto &event : closed) {
                        if (event.plugin_name == udp_plugin_name && event.transport == "udp") {
                            udp_seen_closed = true;
                            break;
                        }
                    }
                    ready = udp_seen_faulted && udp_seen_closed;
                }

                if (ready) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            require(sent_oversized,
                    "protocol connection event udp fault path should send oversized payload");

            adapter.stop();

            const auto stats_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < stats_deadline) {
                udp_fault_stats = adapter.runtime_stats();
                if (udp_fault_stats.accepted_connection_count > 0 &&
                    udp_fault_stats.closed_connection_count > 0 &&
                    udp_fault_stats.active_connection_count == 0 &&
                    udp_fault_stats.framing_error_count > 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        const auto event_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < event_deadline) {
            bool ready = false;
            {
                std::lock_guard<std::mutex> lock(event_mutex);
                bool tcp_ready = false;
                for (const auto &event : accepted) {
                    if (event.plugin_name == "LineEchoProtocol" && event.transport == "tcp") {
                        tcp_ready = true;
                    }
                }
                bool tcp_faulted = false;
                bool udp_faulted = false;
                for (const auto &event : faulted) {
                    if (event.plugin_name == "LineEchoProtocol" && event.transport == "tcp") {
                        tcp_faulted = true;
                    }
                    if (event.plugin_name == udp_plugin_name &&
                        event.transport == "udp" &&
                        event.reason.find("udp frame too large") != std::string::npos) {
                        udp_faulted = true;
                    }
                }
                bool tcp_closed = false;
                bool udp_closed = false;
                for (const auto &event : closed) {
                    if (event.plugin_name == "LineEchoProtocol" && event.transport == "tcp") {
                        tcp_closed = true;
                    }
                    if (event.plugin_name == udp_plugin_name && event.transport == "udp") {
                        udp_closed = true;
                    }
                }
                ready = tcp_ready && tcp_faulted && tcp_closed && udp_faulted && udp_closed;
            }
            if (ready) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(event_mutex);
            require(!accepted.empty(), "protocol connection accepted events should be emitted");
            require(!closed.empty(), "protocol connection closed events should be emitted");
            require(!faulted.empty(), "protocol connection faulted events should be emitted");

            const auto tcp_accepted_it = std::find_if(
                accepted.begin(),
                accepted.end(),
                [](const auto &event) {
                    return event.plugin_name == "LineEchoProtocol" && event.transport == "tcp";
                });
            require(tcp_accepted_it != accepted.end(), "tcp accepted event should be emitted");
            const auto &accepted_event = *tcp_accepted_it;
            require(accepted_event.protocol_service_name == "line_echo" &&
                        accepted_event.host == "127.0.0.1",
                    "accepted event should carry service endpoint identity");
            require(accepted_event.connection_id != 0,
                    "accepted event should carry non-zero connection id for tcp");
            require(!accepted_event.peer_address.empty() && !accepted_event.local_address.empty(),
                    "accepted event should carry peer/local addresses");
            require(accepted_event.active_service_name == "proto.connection.events" &&
                        accepted_event.worker_index == 5 &&
                        accepted_event.service_instance_index == 2,
                    "accepted event should carry runtime service identity");

            const auto tcp_faulted_it = std::find_if(
                faulted.begin(),
                faulted.end(),
                [](const auto &event) {
                    return event.plugin_name == "LineEchoProtocol" && event.transport == "tcp";
                });
            require(tcp_faulted_it != faulted.end(), "tcp faulted event should be emitted");
            const auto &faulted_event = *tcp_faulted_it;
            require(faulted_event.connection_id != 0,
                    "faulted event should carry non-zero connection id for tcp");
            require(!faulted_event.reason.empty(),
                    "faulted event should include reason");
            require(faulted_event.reason_code == "runtime_handler_error",
                    "tcp faulted event should carry runtime handler reason_code");
            require(faulted_event.active_service_name == "proto.connection.events" &&
                        faulted_event.worker_index == 5 &&
                        faulted_event.service_instance_index == 2,
                    "faulted event should carry runtime service identity");

            const auto tcp_closed_it = std::find_if(
                closed.begin(),
                closed.end(),
                [&](const auto &event) {
                    return event.plugin_name == "LineEchoProtocol" &&
                           event.transport == "tcp" &&
                           event.connection_id == faulted_event.connection_id;
                });
            require(tcp_closed_it != closed.end(),
                    "closed event should be emitted for tcp faulted connection");
            const auto &closed_event = *tcp_closed_it;
            require(closed_event.connection_id == faulted_event.connection_id,
                    "closed event should carry non-zero connection id for tcp");

            const auto find_timeline_index = [&](const std::string &type,
                                                 const yuan::plugin::PluginProtocolConnectionEvent &target) {
                for (std::size_t i = 0; i < timeline.size(); ++i) {
                    if (timeline[i].first != type) {
                        continue;
                    }
                    const auto &event = timeline[i].second;
                    if (event.plugin_name == target.plugin_name &&
                        event.transport == target.transport &&
                        event.connection_id == target.connection_id &&
                        event.peer_address == target.peer_address &&
                        event.local_address == target.local_address) {
                        return static_cast<int>(i);
                    }
                }
                return -1;
            };

            const int tcp_accepted_index = find_timeline_index("accepted", faulted_event);
            const int tcp_faulted_index = find_timeline_index("faulted", faulted_event);
            const int tcp_closed_index = find_timeline_index("closed", closed_event);
            require(tcp_accepted_index >= 0 && tcp_faulted_index >= 0 && tcp_closed_index >= 0,
                    "tcp faulted connection should have accepted/faulted/closed timeline entries");
            require(tcp_accepted_index < tcp_faulted_index && tcp_faulted_index < tcp_closed_index,
                    "tcp faulted connection events should be ordered accepted -> faulted -> closed");

            const auto udp_faulted_it = std::find_if(
                faulted.begin(),
                faulted.end(),
                [&](const auto &event) {
                    return event.plugin_name == udp_plugin_name &&
                           event.transport == "udp" &&
                           event.reason.find("udp frame too large") != std::string::npos;
                });
            require(udp_faulted_it != faulted.end(), "udp faulted event should be emitted");
            const auto &udp_faulted_event = *udp_faulted_it;
            require(udp_faulted_event.connection_id == 0,
                    "udp faulted event should carry zero connection id");
            require(!udp_faulted_event.peer_address.empty() && !udp_faulted_event.local_address.empty(),
                    "udp faulted event should carry peer/local addresses");
            require(udp_faulted_event.reason_code == "udp_frame_too_large",
                    "udp faulted event should carry udp frame-too-large reason_code");
            require(udp_faulted_event.worker_index == 5 && udp_faulted_event.service_instance_index == 2,
                    "udp faulted event should carry runtime worker/service_instance identity");

            const auto udp_closed_it = std::find_if(
                closed.begin(),
                closed.end(),
                [&](const auto &event) {
                    return event.plugin_name == udp_plugin_name &&
                           event.transport == "udp" &&
                           event.peer_address == udp_faulted_event.peer_address &&
                           event.local_address == udp_faulted_event.local_address;
                });
            require(udp_closed_it != closed.end(),
                    "udp closed event should be emitted for faulted peer");

            const int udp_faulted_index = find_timeline_index("faulted", udp_faulted_event);
            const int udp_closed_index = find_timeline_index("closed", *udp_closed_it);
            require(udp_faulted_index >= 0 && udp_closed_index >= 0,
                    "udp faulted peer should have faulted/closed timeline entries");
            require(udp_faulted_index < udp_closed_index,
                    "udp faulted peer events should be ordered faulted -> closed");

            require(tcp_ok_stats.accepted_connection_count > 0 &&
                        tcp_ok_stats.closed_connection_count > 0 &&
                        tcp_ok_stats.active_connection_count == 0,
                    "tcp ok adapter runtime stats should include accepted/closed and no active connections");
            require(tcp_ok_stats.bytes_received > 0 && tcp_ok_stats.bytes_sent > 0,
                    "tcp ok adapter runtime stats should include read/write bytes");

            require(tcp_fault_stats.accepted_connection_count > 0 &&
                        tcp_fault_stats.closed_connection_count > 0 &&
                        tcp_fault_stats.active_connection_count == 0,
                    "tcp fault adapter runtime stats should include accepted/closed and no active connections");
            require(tcp_fault_stats.bytes_received > 0,
                    "tcp fault adapter runtime stats should include received bytes");
            require(tcp_fault_stats.handler_error_count > 0,
                    "tcp fault adapter runtime stats should include handler errors");

            require(udp_fault_stats.accepted_connection_count > 0 &&
                        udp_fault_stats.closed_connection_count > 0 &&
                        udp_fault_stats.active_connection_count == 0,
                    "udp fault adapter runtime stats should include accepted/closed and no active peers");
            require(udp_fault_stats.bytes_received > 0,
                    "udp fault adapter runtime stats should include received bytes");
            require(udp_fault_stats.framing_error_count > 0,
                    "udp fault adapter runtime stats should include framing errors");
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_capability_enforcement()
    {
        yuan::plugin::PluginContext ctx;
        ctx.plugin_name = "cap-test";
        ctx.granted_permissions = yuan::plugin::PluginPermission::none;

        require(!ctx.can_use(yuan::plugin::PluginPermission::use_event_bus),
                "plugin with none permission should not be able to use event_bus");

        ctx.granted_permissions = yuan::plugin::PluginPermission::use_event_bus;
        require(ctx.can_use(yuan::plugin::PluginPermission::use_event_bus),
                "plugin with use_event_bus should be able to use event_bus");
        require(!ctx.can_use(yuan::plugin::PluginPermission::use_scheduler),
                "plugin with only use_event_bus should not be able to use scheduler");

        FakeEventBus event_bus;
        ctx.event_bus = &event_bus;
        require(ctx.has_capability(yuan::plugin::PluginPermission::use_event_bus, ctx.event_bus),
                "has_capability should return true when permission granted and pointer set");
        require(!ctx.has_capability(yuan::plugin::PluginPermission::use_scheduler, ctx.scheduler),
                "has_capability should return false when pointer is null");

        FakeScheduler sched;
        ctx.scheduler = &sched;
        ctx.granted_permissions = yuan::plugin::PluginPermission::use_event_bus;
        require(!ctx.has_capability(yuan::plugin::PluginPermission::use_scheduler, ctx.scheduler),
                "has_capability should return false when pointer is set but permission not granted");
    }

    void test_plugin_context_identity_boundary()
    {
        yuan::plugin::PluginContext ctx;
        ctx.app_name = "plugin-identity-app";
        ctx.plugin_name = "identity-plugin";
        ctx.plugin_root_path = "/plugins/identity-plugin";
        ctx.plugin_config_path = "/plugins/identity-plugin/plugin.json";
        ctx.run_mode = yuan::plugin::PluginRunMode::multi_process;
        ctx.worker_threads = 8;
        ctx.runtime_worker_count = 4;
        ctx.worker_index = 2;
        ctx.is_worker_process = true;
        ctx.active_service_name = "plugin-host";
        ctx.service_index = 5;
        ctx.service_instance_index = 1;
        ctx.service_instance_count = 3;
        ctx.listener_reuse_port = true;

        auto boundary = ctx.sdk_boundary();
        require(boundary.app_name == ctx.app_name, "sdk boundary should carry app name");
        require(boundary.plugin_name == ctx.plugin_name, "sdk boundary should carry plugin name");
        require(boundary.worker_threads == 8 &&
                    boundary.runtime_worker_count == 4 &&
                    boundary.worker_index == 2 &&
                    boundary.is_worker_process,
                "sdk boundary should carry runtime worker identity");
        require(boundary.active_service_name == "plugin-host" &&
                    boundary.service_index == 5 &&
                    boundary.service_instance_index == 1 &&
                    boundary.service_instance_count == 3 &&
                    boundary.listener_reuse_port,
                "sdk boundary should carry service-instance identity");
    }

    void test_protocol_service_permission_names()
    {
        const auto permissions = yuan::plugin::PluginPermissionNames::parse(
            "register_protocol_service,listen_tcp,listen_udp,open_outbound_connection,bind_privileged_port,use_tls,use_network_runtime");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::register_protocol_service),
                "register_protocol_service permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::listen_tcp),
                "listen_tcp permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::listen_udp),
                "listen_udp permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::bind_privileged_port),
                "bind_privileged_port permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::open_outbound_connection),
                "open_outbound_connection permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::use_tls),
                "use_tls permission should parse");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::use_network_runtime),
                "use_network_runtime permission should still parse with protocol service permission");
        require(std::string(yuan::plugin::PluginPermissionNames::name(
                    yuan::plugin::PluginPermission::register_protocol_service)) == "register_protocol_service",
                "register_protocol_service permission should have a stable name");

        bool found = false;
        for (const auto &name : yuan::plugin::PluginPermissionNames::to_names(permissions)) {
            if (name == "register_protocol_service") {
                found = true;
            }
        }
        require(found, "to_names should include register_protocol_service");

        bool found_listen_tcp = false;
        bool found_listen_udp = false;
        bool found_open_outbound_connection = false;
        bool found_bind_privileged_port = false;
        bool found_use_tls = false;
        for (const auto &name : yuan::plugin::PluginPermissionNames::to_names(permissions)) {
            if (name == "listen_tcp") {
                found_listen_tcp = true;
            }
            if (name == "listen_udp") {
                found_listen_udp = true;
            }
            if (name == "open_outbound_connection") {
                found_open_outbound_connection = true;
            }
            if (name == "bind_privileged_port") {
                found_bind_privileged_port = true;
            }
            if (name == "use_tls") {
                found_use_tls = true;
            }
        }
        require(found_listen_tcp, "to_names should include listen_tcp");
        require(found_listen_udp, "to_names should include listen_udp");
        require(found_open_outbound_connection, "to_names should include open_outbound_connection");
        require(found_bind_privileged_port, "to_names should include bind_privileged_port");
        require(found_use_tls, "to_names should include use_tls");
    }

    void test_protocol_handler_registry_datagram_groundwork()
    {
        class NoopDatagramHandler final : public yuan::plugin::PluginDatagramProtocolHandler
        {
        public:
            bool on_datagram(yuan::plugin::HostDatagramEndpoint &,
                             std::string_view,
                             std::span<const std::byte>) override
            {
                return true;
            }
        };

        yuan::plugin::PluginProtocolHandlerRegistry registry;
        require(registry.stream_handler_count() == 0,
                "protocol handler registry should start with no stream handlers");
        require(registry.datagram_handler_count() == 0,
                "protocol handler registry should start with no datagram handlers");

        const bool registered = registry.register_datagram_handler(
            "udp.echo",
            [](const yuan::plugin::ProtocolServiceDescriptor &) {
                return std::make_unique<NoopDatagramHandler>();
            });
        require(registered, "datagram handler registration should succeed");
        require(registry.has_datagram_handler("udp.echo"),
                "registry should report registered datagram handler");
        require(registry.datagram_handler_count() == 1,
                "registry should track one datagram handler after registration");

        auto datagram_factory = registry.find_datagram_handler("udp.echo");
        require(static_cast<bool>(datagram_factory),
                "registry should return datagram handler factory by name");

        auto missing_factory = registry.find_datagram_handler("udp.missing");
        require(!missing_factory,
                "registry should return empty datagram factory for missing handler");

        registry.clear();
        require(registry.datagram_handler_count() == 0,
                "registry.clear should remove datagram handlers");
        require(registry.stream_handler_count() == 0,
                "registry.clear should keep stream handler set empty");
    }

    void test_protocol_service_manifest_discovery()
    {
        yuan::plugin::PluginManager manager;

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-services-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto allowed_dir = temp_root / "proto_allowed";
        const auto denied_dir = temp_root / "proto_denied";
        const auto tcp_no_listen_dir = temp_root / "proto_tcp_no_listen";
        const auto udp_no_listen_dir = temp_root / "proto_udp_no_listen";
        const auto privileged_no_bind_dir = temp_root / "proto_privileged_no_bind";
        const auto privileged_with_bind_dir = temp_root / "proto_privileged_with_bind";
        const auto wildcard_no_bind_dir = temp_root / "proto_wildcard_no_bind";
        const auto wildcard_with_bind_dir = temp_root / "proto_wildcard_with_bind";
        const auto wildcard_ipv6_no_bind_dir = temp_root / "proto_wildcard_ipv6_no_bind";
        const auto wildcard_ipv6_with_bind_dir = temp_root / "proto_wildcard_ipv6_with_bind";
        const auto invalid_dir = temp_root / "proto_invalid";
        std::filesystem::create_directories(allowed_dir);
        std::filesystem::create_directories(denied_dir);
        std::filesystem::create_directories(tcp_no_listen_dir);
        std::filesystem::create_directories(udp_no_listen_dir);
        std::filesystem::create_directories(privileged_no_bind_dir);
        std::filesystem::create_directories(privileged_with_bind_dir);
        std::filesystem::create_directories(wildcard_no_bind_dir);
        std::filesystem::create_directories(wildcard_with_bind_dir);
        std::filesystem::create_directories(wildcard_ipv6_no_bind_dir);
        std::filesystem::create_directories(wildcard_ipv6_with_bind_dir);
        std::filesystem::create_directories(invalid_dir);

        auto write_manifest = [](const std::filesystem::path &dir,
                                 const nlohmann::json &doc,
                                 const std::string &message) {
            std::ofstream manifest(dir / "plugin.json");
            require(static_cast<bool>(manifest), message);
            manifest << doc.dump(2) << "\n";
        };

        write_manifest(
            allowed_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "echo_proto"},
                        {"type", "custom"},
                        {"protocol", "tcp"},
                        {"transport", "tcp"},
                        {"host", "127.0.0.1"},
                        {"port", 19090},
                        {"handler", "main.on_connection"},
                        {"framing", "line"},
                        {"read_timeout_ms", 31000},
                        {"idle_timeout_ms", 62000},
                        {"write_timeout_ms", 32000},
                        {"max_connections", 2048},
                        {"max_frame_bytes", 32768},
                        {"max_write_buffer_bytes", 131072},
                        {"contract_id", "plugin.echo"},
                        {"contract_version", 2},
                    },
                })},
            },
            "allowed protocol service manifest should be creatable");

        write_manifest(
            denied_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "blocked_proto"},
                        {"protocol", "tcp"},
                        {"port", 19091},
                        {"contract_id", "plugin.blocked"},
                    },
                })},
            },
            "denied protocol service manifest should be creatable");

        write_manifest(
            tcp_no_listen_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "tcp_no_listen"},
                        {"transport", "tcp"},
                        {"port", 19092},
                        {"contract_id", "plugin.tcp.no_listen"},
                    },
                })},
            },
            "tcp no-listen manifest should be creatable");

        write_manifest(
            udp_no_listen_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "udp_no_listen"},
                        {"transport", "udp"},
                        {"framing", "raw"},
                        {"port", 19093},
                        {"contract_id", "plugin.udp.no_listen"},
                    },
                })},
            },
            "udp no-listen manifest should be creatable");

        write_manifest(
            privileged_no_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "privileged_no_bind"},
                        {"transport", "tcp"},
                        {"port", 443},
                        {"contract_id", "plugin.privileged.no_bind"},
                    },
                })},
            },
            "privileged no-bind manifest should be creatable");

        write_manifest(
            privileged_with_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,bind_privileged_port,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "privileged_with_bind"},
                        {"transport", "tcp"},
                        {"port", 443},
                        {"contract_id", "plugin.privileged.with_bind"},
                    },
                })},
            },
            "privileged with-bind manifest should be creatable");

        write_manifest(
            wildcard_no_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "wildcard_no_bind"},
                        {"transport", "tcp"},
                        {"host", "0.0.0.0"},
                        {"port", 19094},
                        {"contract_id", "plugin.wildcard.no_bind"},
                    },
                })},
            },
            "wildcard no-bind manifest should be creatable");

        write_manifest(
            wildcard_with_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,bind_privileged_port,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "wildcard_with_bind"},
                        {"transport", "tcp"},
                        {"host", "0.0.0.0"},
                        {"port", 19095},
                        {"contract_id", "plugin.wildcard.with_bind"},
                    },
                })},
            },
            "wildcard with-bind manifest should be creatable");

        write_manifest(
            wildcard_ipv6_no_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "wildcard_ipv6_no_bind"},
                        {"transport", "tcp"},
                        {"host", "::"},
                        {"port", 19096},
                        {"contract_id", "plugin.wildcard.ipv6.no_bind"},
                    },
                })},
            },
            "wildcard ipv6 no-bind manifest should be creatable");

        write_manifest(
            wildcard_ipv6_with_bind_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,bind_privileged_port,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "wildcard_ipv6_with_bind"},
                        {"transport", "tcp"},
                        {"host", "::"},
                        {"port", 19097},
                        {"contract_id", "plugin.wildcard.ipv6.with_bind"},
                    },
                })},
            },
            "wildcard ipv6 with-bind manifest should be creatable");

        write_manifest(
            invalid_dir,
            {
                {"run_mode", "script"},
                {"language", "lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "bad_proto"},
                        {"transport", "tcp"},
                        {"port", 70000},
                        {"contract_id", "plugin.bad"},
                    },
                })},
            },
            "invalid protocol service manifest should be creatable");

        manager.set_plugin_path(temp_root.string());

        const auto allowed = manager.discover_protocol_services({ "proto_allowed" });
        require(allowed.size() == 1, "plugin manager should discover allowed protocol service");
        require(allowed[0].plugin_id == "proto_allowed", "protocol service should carry plugin id");
        require(allowed[0].name == "echo_proto", "protocol service should carry name");
        require(allowed[0].type == "custom", "protocol service should carry type");
        require(allowed[0].protocol == "tcp", "protocol service should carry protocol");
        require(allowed[0].transport == "tcp", "protocol service should carry transport");
        require(allowed[0].host == "127.0.0.1", "protocol service should carry host");
        require(allowed[0].port == 19090, "protocol service should carry port");
        require(allowed[0].handler == "main.on_connection", "protocol service should carry handler");
        require(allowed[0].framing == "line", "protocol service should carry framing");
        require(allowed[0].read_timeout_ms == 31000, "protocol service should carry read timeout");
        require(allowed[0].idle_timeout_ms == 62000, "protocol service should carry idle timeout");
        require(allowed[0].write_timeout_ms == 32000, "protocol service should carry write timeout");
        require(allowed[0].max_connections == 2048, "protocol service should carry max connections");
        require(allowed[0].max_frame_bytes == 32768, "protocol service should carry max frame size");
        require(allowed[0].max_write_buffer_bytes == 131072, "protocol service should carry max write buffer size");
        require(allowed[0].contract_id == "plugin.echo", "protocol service should carry contract id");
        require(allowed[0].contract_version == 2, "protocol service should carry contract version");
        require(allowed[0].run_mode == yuan::plugin::PluginRunMode::script,
                "protocol service should carry plugin run mode");
        require(allowed[0].language == "lua", "protocol service should carry language");
        require(allowed[0].entry == "main.lua", "protocol service should carry entry");

        const auto denied = manager.discover_protocol_services({ "proto_denied" });
        require(denied.empty(), "plugin manager should reject protocol services without permission");

        const auto tcp_no_listen = manager.discover_protocol_services({ "proto_tcp_no_listen" });
        require(tcp_no_listen.empty(), "plugin manager should reject tcp protocol services without listen_tcp permission");

        const auto udp_no_listen = manager.discover_protocol_services({ "proto_udp_no_listen" });
        require(udp_no_listen.empty(), "plugin manager should reject udp protocol services without listen_udp permission");

        const auto privileged_no_bind = manager.discover_protocol_services({ "proto_privileged_no_bind" });
        require(privileged_no_bind.empty(), "plugin manager should reject privileged port without bind_privileged_port permission");

        const auto privileged_with_bind = manager.discover_protocol_services({ "proto_privileged_with_bind" });
        require(privileged_with_bind.size() == 1,
                "plugin manager should allow privileged port with bind_privileged_port permission");

        const auto wildcard_no_bind = manager.discover_protocol_services({ "proto_wildcard_no_bind" });
        require(wildcard_no_bind.empty(), "plugin manager should reject wildcard host without bind_privileged_port permission");

        const auto wildcard_with_bind = manager.discover_protocol_services({ "proto_wildcard_with_bind" });
        require(wildcard_with_bind.size() == 1,
                "plugin manager should allow wildcard host when bind_privileged_port permission is granted");

        const auto wildcard_ipv6_no_bind = manager.discover_protocol_services({ "proto_wildcard_ipv6_no_bind" });
        require(wildcard_ipv6_no_bind.empty(),
                "plugin manager should reject wildcard ipv6 host without bind_privileged_port permission");

        const auto wildcard_ipv6_with_bind = manager.discover_protocol_services({ "proto_wildcard_ipv6_with_bind" });
        require(wildcard_ipv6_with_bind.size() == 1,
                "plugin manager should allow wildcard ipv6 host when bind_privileged_port permission is granted");

        const auto invalid = manager.discover_protocol_services({ "proto_invalid" });
        require(invalid.empty(), "plugin manager should reject protocol services with invalid numeric ranges");

        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::sharded;
        placement.instances = 2;
        const auto descriptor = yuan::app::make_plugin_protocol_service_descriptor(allowed[0], placement);
        require(descriptor.has_value(), "allowed protocol service should convert to app service descriptor");
        require(descriptor->name == "proto_allowed.echo_proto",
                "plugin protocol descriptor should be namespaced by plugin id");
        require(descriptor->type_name == "plugin.protocol.custom",
                "plugin protocol descriptor should carry plugin protocol type");
        require(descriptor->contract_id == "plugin.echo",
                "plugin protocol descriptor should carry contract id");
        require(descriptor->contract_version == 2,
                "plugin protocol descriptor should carry contract version");
        require(descriptor->placement.mode == yuan::app::PlacementMode::sharded &&
                    descriptor->placement.instances == 2,
                "plugin protocol descriptor should preserve requested placement");
        require(descriptor->endpoints.size() == 1,
                "plugin protocol descriptor should expose one endpoint");
        require(descriptor->endpoints[0].name == "echo_proto" &&
                    descriptor->endpoints[0].host == "127.0.0.1" &&
                    descriptor->endpoints[0].port == 19090 &&
                    descriptor->endpoints[0].protocol == "tcp",
                "plugin protocol descriptor should carry endpoint details");

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_worker_local_adapter()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-worker-local-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_worker";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "worker-local protocol plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "echo_proto"},
                        {"type", "echo"},
                        {"protocol", "tcp"},
                        {"host", "127.0.0.1"},
                        {"port", 0},
                        {"contract_id", "plugin.echo.worker"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "worker-local protocol plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx)\n";
            script << "  if not ctx.logger then return false end\n";
            script << "  ctx.logger:info('protocol worker init')\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_worker" });
        require(protocol_services.size() == 1, "worker-local protocol service should be discoverable");

        auto event_bus = std::make_shared<yuan::eventbus::EventBus>();
        std::mutex mutex;
        std::vector<std::size_t> loaded_worker_indices;
        std::vector<std::size_t> loaded_service_indices;

        event_bus->subscribe(yuan::plugin::events::plugin_loaded, [&](const yuan::eventbus::Event &event) {
            const auto *plugin_event = std::any_cast<yuan::plugin::PluginEvent>(&event.payload);
            if (!plugin_event || plugin_event->plugin_name != "proto_worker") {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex);
            loaded_worker_indices.push_back(plugin_event->worker_index);
            loaded_service_indices.push_back(plugin_event->service_instance_index);
        });

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-worker-local-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 2;
        context.runtime_workers.worker_count = 2;
        context.event_bus = event_bus;

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::all_workers;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    temp_root.string(),
                    protocol_services[0],
                    placement),
                "plugin protocol service should register as an app ServiceDefinition");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start worker-local plugin protocol service instances");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (loaded_worker_indices.size() >= 2) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto snapshot = bootstrap.supervisor_snapshot();
        require(snapshot.running_workers == 2,
                "plugin protocol service should run on two in-process workers");

        {
            std::lock_guard<std::mutex> lock(mutex);
            require(loaded_worker_indices.size() == 2,
                    "each worker-local plugin protocol service should initialize a plugin runtime");
            std::sort(loaded_worker_indices.begin(), loaded_worker_indices.end());
            std::sort(loaded_service_indices.begin(), loaded_service_indices.end());
            require(loaded_worker_indices[0] == 0 && loaded_worker_indices[1] == 1,
                    "plugin loaded events should carry distinct worker indices");
            require(loaded_service_indices[0] == 0 && loaded_service_indices[1] == 1,
                    "plugin loaded events should carry distinct service instance indices");
        }

        bootstrap.shutdown();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_echo_listener()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "plugin protocol echo test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-echo-listener-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_echo";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "echo protocol plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "echo_proto"},
                        {"type", "echo"},
                        {"protocol", "tcp"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"contract_id", "plugin.echo.listener"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "echo protocol plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx)\n";
            script << "  if not ctx.logger then return false end\n";
            script << "  ctx.logger:info('protocol echo listener init')\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_echo" });
        require(protocol_services.size() == 1, "echo protocol service should be discoverable");

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-echo-listener-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    temp_root.string(),
                    protocol_services[0],
                    placement),
                "echo plugin protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start plugin protocol echo listener");
        require(tcp_echo_roundtrip(port, "plugin-echo-payload"),
                "plugin protocol echo listener should serve a TCP roundtrip");
        bootstrap.shutdown();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_multi_worker_reuse_port_identity_isolation()
    {
#ifdef _WIN32
        std::cout << "protocol multi-worker reuse_port test skipped on Windows\n";
        return;
#endif

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "multi-worker reuse_port protocol test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-reuse-port-identity-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        write_script_protocol_plugin(
            temp_root,
            "proto_reuse_port",
            {
                {"name", "reuse_port_proto"},
                {"type", "echo"},
                {"transport", "tcp"},
                {"host", "127.0.0.1"},
                {"port", port},
                {"contract_id", "plugin.reuse.port.identity"},
            });

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_reuse_port" });
        require(protocol_services.size() == 1,
                "multi-worker reuse_port protocol service should be discoverable");

        auto event_bus = std::make_shared<yuan::eventbus::EventBus>();
        std::mutex mutex;
        std::vector<std::size_t> loaded_worker_indices;
        std::vector<std::size_t> loaded_service_indices;
        std::vector<yuan::plugin::PluginProtocolServiceEvent> started_events;
        std::vector<yuan::plugin::PluginProtocolConnectionEvent> accepted_events;

        event_bus->subscribe(yuan::plugin::events::plugin_loaded,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginEvent>(&event.payload);
                                 if (!payload || payload->plugin_name != "proto_reuse_port") {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(mutex);
                                 loaded_worker_indices.push_back(payload->worker_index);
                                 loaded_service_indices.push_back(payload->service_instance_index);
                             });

        event_bus->subscribe(yuan::plugin::events::plugin_protocol_service_started,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolServiceEvent>(&event.payload);
                                 if (!payload || payload->plugin_name != "proto_reuse_port") {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(mutex);
                                 started_events.push_back(*payload);
                             });
        event_bus->subscribe(yuan::plugin::events::plugin_protocol_connection_accepted,
                             [&](const yuan::eventbus::Event &event) {
                                 const auto *payload = std::any_cast<yuan::plugin::PluginProtocolConnectionEvent>(&event.payload);
                                 if (!payload || payload->plugin_name != "proto_reuse_port") {
                                     return;
                                 }
                                 std::lock_guard<std::mutex> lock(mutex);
                                 accepted_events.push_back(*payload);
                             });

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-reuse-port-identity-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 2;
        context.runtime_workers.worker_count = 2;
        context.event_bus = event_bus;

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::all_workers;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    temp_root.string(),
                    protocol_services[0],
                    placement),
                "multi-worker reuse_port service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "multi-worker reuse_port bootstrap should start");

        const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < start_deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (started_events.size() >= 2 && loaded_worker_indices.size() >= 2) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        for (int i = 0; i < 8; ++i) {
            require(tcp_echo_roundtrip(port, "reuse-port-identity-" + std::to_string(i)),
                    "multi-worker reuse_port listener should serve tcp roundtrip");
        }

        const auto accepted_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < accepted_deadline) {
            bool seen_accepted = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                seen_accepted = !accepted_events.empty();
            }
            if (seen_accepted) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            require(loaded_worker_indices.size() == 2,
                    "multi-worker reuse_port should initialize independent plugin runtimes per worker");
            std::sort(loaded_worker_indices.begin(), loaded_worker_indices.end());
            std::sort(loaded_service_indices.begin(), loaded_service_indices.end());
            require(loaded_worker_indices[0] == 0 && loaded_worker_indices[1] == 1,
                    "multi-worker reuse_port plugin loaded events should carry distinct worker indices");
            require(loaded_service_indices[0] == 0 && loaded_service_indices[1] == 1,
                    "multi-worker reuse_port plugin loaded events should carry distinct service instance indices");

            require(started_events.size() >= 2,
                    "multi-worker reuse_port should emit started events for each worker instance");

            std::set<std::size_t> started_workers;
            std::set<std::size_t> started_instances;
            for (const auto &event : started_events) {
                started_workers.insert(event.worker_index);
                started_instances.insert(event.service_instance_index);
                require(event.listener_reuse_port,
                        "multi-worker reuse_port started event should carry listener_reuse_port=true");
                require(event.service_instance_count == 2,
                        "multi-worker reuse_port started event should carry service_instance_count");
            }
            require(started_workers.size() >= 2,
                    "multi-worker reuse_port should start on distinct workers");
            require(started_instances.size() >= 2,
                    "multi-worker reuse_port should use distinct service instances");

            require(!accepted_events.empty(),
                    "multi-worker reuse_port should emit connection accepted events");
            for (const auto &event : accepted_events) {
                require(event.listener_reuse_port,
                        "multi-worker reuse_port accepted event should carry listener_reuse_port=true");
                require(event.service_instance_count == 2,
                        "multi-worker reuse_port accepted event should carry service_instance_count");
            }
        }

        bootstrap.shutdown();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_lua_custom_line_echo_listener()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua custom protocol test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lua-custom-listener-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lua_custom";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua custom protocol plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "line_proto"},
                        {"type", "custom"},
                        {"transport", "tcp"},
                        {"framing", "line"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"handler", "main.on_connection"},
                        {"contract_id", "plugin.lua.custom.line"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "lua custom protocol plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx)\n";
            script << "  if not ctx.logger then return false end\n";
            script << "  ctx.logger:info('lua custom protocol init')\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_connection(conn, data)\n";
            script << "  conn:write(data)\n";
            script << "  conn:write('\\n')\n";
            script << "  conn:flush()\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lua_custom" });
        require(protocol_services.size() == 1, "lua custom protocol service should be discoverable");

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-custom-listener-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    temp_root.string(),
                    protocol_services[0],
                    placement),
                "lua custom protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start lua custom protocol listener");
        require(tcp_echo_roundtrip(port, "lua-custom-line\n"),
                "lua custom protocol listener should serve TCP line roundtrip");
        bootstrap.shutdown();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_lua_handler_returns_false()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua handler-false test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lua-return-false-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lua_return_false";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua return-false plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "line_proto"},
                        {"type", "custom"},
                        {"transport", "tcp"},
                        {"framing", "line"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"handler", "main.on_connection"},
                        {"contract_id", "plugin.lua.custom.return_false"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "lua return-false plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_connection(conn, data)\n";
            script << "  return false\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lua_return_false" });
        require(protocol_services.size() == 1, "lua return-false protocol service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-handler-return-false-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "lua return-false adapter should initialize");
        adapter.start();
        require(adapter.started(), "lua return-false adapter should start listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "lua return-false test client should connect");
        require(tcp_send(client, "stop\n"), "lua return-false test should send payload");
        require(wait_for_socket_close(client, std::chrono::seconds(2)),
                "lua handler returning false should close the connection");

        auto stats = adapter.runtime_stats();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (stats.handler_error_count == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stats = adapter.runtime_stats();
        }
        require(stats.handler_error_count > 0,
                "lua handler returning false should increment handler error count");

        close_socket(client);
        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_lua_handler_error()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua handler-error test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lua-handler-error-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lua_handler_error";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua handler-error plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "line_proto"},
                        {"type", "custom"},
                        {"transport", "tcp"},
                        {"framing", "line"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"handler", "main.on_connection"},
                        {"contract_id", "plugin.lua.custom.handler_error"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "lua handler-error plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_connection(conn, data)\n";
            script << "  error('lua handler error path')\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lua_handler_error" });
        require(protocol_services.size() == 1, "lua handler-error protocol service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-handler-error-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "lua handler-error adapter should initialize");
        adapter.start();
        require(adapter.started(), "lua handler-error adapter should start listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "lua handler-error test client should connect");
        require(tcp_send(client, "boom\n"), "lua handler-error test should send payload");
        require(wait_for_socket_close(client, std::chrono::seconds(2)),
                "lua handler error should close the connection");

        auto stats = adapter.runtime_stats();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (stats.handler_error_count == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stats = adapter.runtime_stats();
        }
        require(stats.handler_error_count > 0,
                "lua handler error should increment handler error count");

        close_socket(client);
        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_lua_idle_timeout_closes_partial_line_connections()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua idle-timeout test should reserve a TCP port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lua-idle-timeout-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lua_idle_timeout";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua idle-timeout plugin manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_tcp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "line_proto"},
                        {"type", "custom"},
                        {"transport", "tcp"},
                        {"framing", "line"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"handler", "main.on_connection"},
                        {"read_timeout_ms", 0},
                        {"idle_timeout_ms", 200},
                        {"contract_id", "plugin.lua.custom.idle_timeout"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "lua idle-timeout plugin script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_connection(conn, data)\n";
            script << "  conn:write(data)\n";
            script << "  conn:write('\\n')\n";
            script << "  conn:flush()\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lua_idle_timeout" });
        require(protocol_services.size() == 1, "lua idle-timeout protocol service should be discoverable");

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-idle-timeout-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    temp_root.string(),
                    protocol_services[0],
                    placement),
                "lua idle-timeout protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start lua idle-timeout listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "lua idle-timeout test client should connect");
        require(tcp_send(client, "partial-line-without-newline"),
                "lua idle-timeout test should send a partial line payload");

        const auto drain_result = drain_socket_until_close(client, std::chrono::seconds(2));
        require(drain_result.closed, "lua idle-timeout test connection should be closed by the service");
        require(drain_result.data.empty(),
                "lua idle-timeout should not dispatch a partial line frame when the peer goes idle");

        close_socket(client);
        bootstrap.shutdown();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_cpp_line_echo_listener()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol line echo test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        require(protocol_service.type == "custom",
                "LineEchoProtocol should be declared as a custom protocol service");
        require(protocol_service.handler == "line_echo.on_connection",
                "LineEchoProtocol should name its C++ stream handler");
        require(protocol_service.framing == "line",
                "LineEchoProtocol should use line framing");

        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-cpp-line-echo-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "C++ line echo plugin protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start C++ plugin protocol line echo listener");
        require(tcp_echo_roundtrip(port, "cpp-line-echo-payload\n"),
                "C++ plugin protocol line echo listener should serve a TCP line roundtrip");
        bootstrap.shutdown();
    }

    void test_protocol_service_lua_example_line_echo_listener()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "plugin examples directory should be resolvable for lua line echo example");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua example line echo test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "lua_line_echo" });
        require(protocol_services.size() == 1,
                "lua_line_echo manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        require(protocol_service.type == "custom",
                "lua_line_echo should be declared as a custom protocol service");
        require(protocol_service.handler == "main.on_connection",
                "lua_line_echo should declare main.on_connection handler");
        require(protocol_service.framing == "line",
                "lua_line_echo should use line framing");

        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-example-line-echo-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "lua_line_echo protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start lua_line_echo listener");
        require(tcp_echo_roundtrip(port, "lua-example-line-echo\n"),
                "lua_line_echo plugin protocol listener should serve TCP line roundtrip");
        bootstrap.shutdown();
    }

    void test_protocol_service_ts_example_line_echo_listener()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "plugin examples directory should be resolvable for ts line echo example");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "ts example line echo test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "ts_line_echo" });
        require(protocol_services.size() == 1,
                "ts_line_echo manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        require(protocol_service.type == "custom",
                "ts_line_echo should be declared as a custom protocol service");
        require(protocol_service.handler == "main.onConnection",
                "ts_line_echo should declare main.onConnection handler");
        require(protocol_service.framing == "line",
                "ts_line_echo should use line framing");

        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-ts-example-line-echo-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "ts_line_echo protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start ts_line_echo listener");
        require(tcp_echo_roundtrip(port, "ts-example-line-echo\n"),
                "ts_line_echo plugin protocol listener should serve TCP line roundtrip");
        bootstrap.shutdown();
    }

    void test_protocol_service_handler_fault_events()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol fault test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;
        protocol_service.handler = "line_echo.throw_on_data";

        auto event_bus = std::make_shared<yuan::eventbus::EventBus>();
        std::mutex mutex;
        std::vector<yuan::plugin::PluginFaultEvent> fault_events;
        int degraded_events = 0;

        event_bus->subscribe(yuan::plugin::events::plugin_faulted, [&](const yuan::eventbus::Event &event) {
            const auto *fault_event = std::any_cast<yuan::plugin::PluginFaultEvent>(&event.payload);
            if (!fault_event || fault_event->plugin_name != "LineEchoProtocol") {
                return;
            }

            std::lock_guard<std::mutex> lock(mutex);
            fault_events.push_back(*fault_event);
        });

        event_bus->subscribe(yuan::plugin::events::plugin_degraded, [&](const yuan::eventbus::Event &event) {
            const auto *plugin_event = std::any_cast<yuan::plugin::PluginEvent>(&event.payload);
            if (!plugin_event || plugin_event->plugin_name != "LineEchoProtocol") {
                return;
            }

            std::lock_guard<std::mutex> lock(mutex);
            ++degraded_events;
        });

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-fault-event-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = event_bus;

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "C++ line echo plugin protocol fault service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start C++ protocol fault listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "fault test client should connect to the protocol listener");
        require(tcp_send(client, "boom\n"), "fault test payload should be sent");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!fault_events.empty() && degraded_events > 0) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            require(!fault_events.empty(), "handler exception should publish a plugin_faulted event");
            require(degraded_events > 0, "first handler exception should degrade the plugin");
            require(fault_events.back().call_site == "protocol.on_data",
                    "fault event should identify the protocol on_data call site");
            require(fault_events.back().fault_message.find("line echo handler test failure") != std::string::npos,
                    "fault event should include the handler exception message");
        }

        close_socket(client);
        bootstrap.shutdown();
    }

    void test_protocol_service_shutdown_closes_active_connections()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol shutdown test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-close-connection-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "C++ line echo plugin protocol service should register for shutdown test");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start C++ protocol shutdown listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "shutdown test client should connect to the protocol listener");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        bootstrap.shutdown();
        require(wait_for_socket_close(client, std::chrono::seconds(2)),
                "plugin protocol shutdown should close active connections");

        close_socket(client);
    }

    void test_protocol_service_resource_guard_tracking()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol resource tracking test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-resource-guard-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
        adapter.set_runtime_context(context);
        require(adapter.init(), "protocol service adapter should initialize for resource tracking test");

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        adapter.start();
        require(adapter.started(), "protocol service adapter should start for resource tracking test");

        auto report = adapter.resource_leak_report();
        require(report.find("network_listener") != std::string::npos,
                "resource report should include the tracked protocol listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "resource tracking test client should connect to the protocol listener");

        const auto connection_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < connection_deadline) {
            report = adapter.resource_leak_report();
            if (report.find("network_connection") != std::string::npos) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(report.find("network_connection") != std::string::npos,
                "resource report should include the tracked active protocol connection");

        close_socket(client);

        const auto disconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < disconnect_deadline) {
            report = adapter.resource_leak_report();
            if (report.find("network_connection") == std::string::npos) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(report.find("network_connection") == std::string::npos,
                "resource report should stop listing the connection after peer disconnect");

        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
    }

    void test_protocol_service_idle_timeout_closes_partial_line_connections()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol idle-timeout test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;
        protocol_service.read_timeout_ms = 0;
        protocol_service.idle_timeout_ms = 200;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-idle-timeout-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "C++ line echo plugin protocol service should register for idle-timeout test");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start C++ protocol idle-timeout listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "idle-timeout test client should connect to the protocol listener");
        require(tcp_send(client, "partial-line-without-newline"),
                "idle-timeout test should send a partial line payload");

        const auto drain_result = drain_socket_until_close(client, std::chrono::seconds(2));
        require(drain_result.closed, "idle-timeout test connection should be closed by the service");
        require(drain_result.data.empty(),
                "idle-timeout should not dispatch a partial line frame when the peer goes idle");

        close_socket(client);
        bootstrap.shutdown();
    }

    void test_protocol_service_length_prefixed_framing()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol length-prefixed test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;
        protocol_service.handler = "length_echo.on_connection";
        protocol_service.framing = "length_prefixed";
        protocol_service.max_frame_bytes = 4096;

        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-length-prefixed-test";
        context.run_mode = yuan::app::RunMode::multi_thread;
        context.worker_threads = 1;
        context.runtime_workers.worker_count = 1;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        yuan::app::Application app(context);
        yuan::app::ServicePlacement placement;
        placement.mode = yuan::app::PlacementMode::singleton;

        require(yuan::app::add_plugin_protocol_service(
                    app,
                    plugin_examples_dir.string(),
                    protocol_service,
                    placement),
                "C++ length-prefixed protocol service should register");

        yuan::app::Bootstrap bootstrap(app);
        require(bootstrap.run(), "bootstrap should start C++ length-prefixed listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "length-prefixed test client should connect to the listener");

        const auto half_packet = make_length_prefixed_frame("half-packet");
        require(tcp_send(client, half_packet.substr(0, 2)),
                "length-prefixed test should send the partial header");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(tcp_send(client, half_packet.substr(2)),
                "length-prefixed test should send the remainder of the half packet");

        std::string received;
        require(tcp_receive_length_prefixed_frame(client, std::chrono::seconds(2), received),
                "length-prefixed test should receive the half-packet response");
        require(received == "half-packet",
                "length-prefixed framing should reassemble half packets before dispatch");

        std::string sticky_frames;
        sticky_frames += make_length_prefixed_frame("sticky-one");
        sticky_frames += make_length_prefixed_frame("sticky-two");
        require(tcp_send(client, sticky_frames),
                "length-prefixed test should send two frames in one write");

        require(tcp_receive_length_prefixed_frame(client, std::chrono::seconds(2), received),
                "length-prefixed test should receive the first sticky-frame response");
        require(received == "sticky-one",
                "length-prefixed framing should preserve the first sticky frame boundary");
        require(tcp_receive_length_prefixed_frame(client, std::chrono::seconds(2), received),
                "length-prefixed test should receive the second sticky-frame response");
        require(received == "sticky-two",
                "length-prefixed framing should preserve the second sticky frame boundary");

        const std::string large_payload(3072, 'x');
        require(tcp_send(client, make_length_prefixed_frame(large_payload)),
                "length-prefixed test should send a large in-range frame");
        require(tcp_receive_length_prefixed_frame(client, std::chrono::seconds(2), received),
                "length-prefixed test should receive the large-frame response");
        require(received == large_payload,
                "length-prefixed framing should pass through large in-range frames");

        close_socket(client);

        const socket_t oversize_client = connect_tcp_socket(port);
        require(oversize_client != kInvalidSocket,
                "length-prefixed oversize test client should connect to the listener");

        std::string oversize_header;
        append_length_prefix(
            oversize_header,
            static_cast<uint32_t>(protocol_service.max_frame_bytes + 1));
        require(tcp_send(oversize_client, oversize_header),
                "length-prefixed oversize test should send the frame header");
        require(wait_for_socket_close(oversize_client, std::chrono::seconds(2)),
                "length-prefixed oversize frame should close the connection");

        close_socket(oversize_client);
        bootstrap.shutdown();
    }

    void test_protocol_service_runtime_error_stats_split()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-runtime-stats-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = reserve_tcp_port();
            protocol_service.handler = "line_echo.throw_on_data";

            require(protocol_service.port != 0, "handler-error stats test should reserve a TCP port");

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "handler-error stats adapter should initialize");
            adapter.start();
            require(adapter.started(), "handler-error stats adapter should start");

            const socket_t client = connect_tcp_socket(protocol_service.port);
            require(client != kInvalidSocket, "handler-error stats test client should connect");
            require(tcp_send(client, "boom\n"),
                    "handler-error stats test should send the failing payload");

            auto stats = adapter.runtime_stats();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (stats.handler_error_count == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                stats = adapter.runtime_stats();
            }

            require(stats.handler_error_count > 0,
                    "handler failures should increment the handler error count");
            require(stats.framing_error_count == 0,
                    "handler failures should not increment the framing error count");
            require(stats.bytes_received > 0,
                    "handler-error stats should include received bytes");

            close_socket(client);
            adapter.stop();

            const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < settle_deadline) {
                stats = adapter.runtime_stats();
                if (stats.accepted_connection_count > 0 &&
                    stats.closed_connection_count > 0 &&
                    stats.active_connection_count == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(stats.accepted_connection_count > 0 &&
                        stats.closed_connection_count > 0 &&
                        stats.active_connection_count == 0,
                    "handler-error stats should include accepted/closed and no active connections");
        }

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = reserve_tcp_port();
            protocol_service.handler = "length_echo.on_connection";
            protocol_service.framing = "length_prefixed";
            protocol_service.max_frame_bytes = 64;

            require(protocol_service.port != 0, "framing-error stats test should reserve a TCP port");

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "framing-error stats adapter should initialize");
            adapter.start();
            require(adapter.started(), "framing-error stats adapter should start");

            const socket_t client = connect_tcp_socket(protocol_service.port);
            require(client != kInvalidSocket, "framing-error stats test client should connect");

            std::string oversize_header;
            append_length_prefix(
                oversize_header,
                static_cast<uint32_t>(protocol_service.max_frame_bytes + 1));
            require(tcp_send(client, oversize_header),
                    "framing-error stats test should send an oversize frame header");
            auto stats = adapter.runtime_stats();
            bool connection_closed = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < deadline) {
                if (stats.framing_error_count > 0 && connection_closed) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                stats = adapter.runtime_stats();
                if (!connection_closed) {
                    connection_closed = wait_for_socket_close(client, std::chrono::milliseconds(50));
                }
            }

            require(connection_closed,
                    "framing-error stats test connection should be closed");

            require(stats.framing_error_count > 0,
                    "framing failures should increment the framing error count");
            require(stats.handler_error_count == 0,
                    "framing failures should not increment the handler error count");
            require(stats.backpressure_drop_count == 0,
                    "framing failures should not increment the backpressure drop count");
            require(stats.bytes_received > 0,
                    "framing-error stats should include received bytes");

            close_socket(client);
            adapter.stop();

            const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < settle_deadline) {
                stats = adapter.runtime_stats();
                if (stats.accepted_connection_count > 0 &&
                    stats.closed_connection_count > 0 &&
                    stats.active_connection_count == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(stats.accepted_connection_count > 0 &&
                        stats.closed_connection_count > 0 &&
                        stats.active_connection_count == 0,
                    "framing-error stats should include accepted/closed and no active connections");
        }

        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
    }

    void test_protocol_service_backpressure_write_buffer_limit()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "C++ protocol backpressure test should reserve a TCP port");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        auto protocol_service = protocol_services[0];
        protocol_service.host = "127.0.0.1";
        protocol_service.port = port;
        protocol_service.framing = "raw";
        protocol_service.handler = "line_echo.on_connection";
        protocol_service.max_frame_bytes = 32768;
        protocol_service.max_write_buffer_bytes = 1024;

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-backpressure-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
        adapter.set_runtime_context(context);
        require(adapter.init(), "backpressure adapter should initialize");
        adapter.start();
        require(adapter.started(), "backpressure adapter should start listener");

        const socket_t client = connect_tcp_socket(port);
        require(client != kInvalidSocket, "backpressure test client should connect to listener");

        const std::string oversized_payload(protocol_service.max_write_buffer_bytes + 64, 'z');
        require(tcp_send(client, oversized_payload),
                "backpressure test should send an oversized payload for echo write buffer");
        require(wait_for_socket_close(client, std::chrono::seconds(2)),
                "backpressure overflow should close the connection");

        auto stats = adapter.runtime_stats();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (stats.backpressure_drop_count == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            stats = adapter.runtime_stats();
        }

        require(stats.backpressure_drop_count > 0,
                "backpressure overflow should increment backpressure drop count");
        require(stats.bytes_received > 0,
                "backpressure stats should include received bytes");

        close_socket(client);
        adapter.stop();

        const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < settle_deadline) {
            stats = adapter.runtime_stats();
            if (stats.accepted_connection_count > 0 &&
                stats.closed_connection_count > 0 &&
                stats.active_connection_count == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(stats.accepted_connection_count > 0 &&
                    stats.closed_connection_count > 0 &&
                    stats.active_connection_count == 0,
                "backpressure stats should include accepted/closed and no active connections");
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
    }

    void test_protocol_service_health_snapshot()
    {
        const auto plugin_examples_dir = resolve_plugin_examples_dir();
        require(!plugin_examples_dir.empty(),
                "C++ protocol plugin examples directory should be resolvable");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(plugin_examples_dir.string());
        auto protocol_services = discovery_manager.discover_protocol_services({ "LineEchoProtocol" });
        require(protocol_services.size() == 1,
                "LineEchoProtocol manifest should declare one protocol service");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-health-snapshot-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.worker_index = 7;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = reserve_tcp_port();
            protocol_service.handler = "line_echo.on_connection";
            protocol_service.max_connections = 1;
            require(protocol_service.port != 0, "health snapshot connection-limit test should reserve a TCP port");

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "health snapshot connection-limit adapter should initialize");
            adapter.start();
            require(adapter.started(), "health snapshot connection-limit adapter should start");

            auto health = adapter.health_snapshot();
            require(health.listener_present,
                    "health snapshot should report listener present after start");
            require(health.max_connection_limit == 1,
                    "health snapshot should report configured max connection limit");
            require(health.active_connection_count == 0 &&
                        !health.connection_at_capacity &&
                        !health.connection_over_limit &&
                        health.healthy,
                    "health snapshot should be healthy with no active connections");

            const socket_t client = connect_tcp_socket(protocol_service.port);
            require(client != kInvalidSocket, "health snapshot test client should connect");

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                health = adapter.health_snapshot();
                if (health.active_connection_count > 0 && health.connection_at_capacity) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            require(health.active_connection_count > 0,
                    "health snapshot should report active connection while client is connected");
            require(health.connection_at_capacity,
                    "health snapshot should report at-capacity when active reaches max");
            require(!health.connection_over_limit,
                    "health snapshot should not report strict over-limit at exact max");
            require(!health.healthy,
                    "health snapshot should report unhealthy when connection limit is saturated");

            close_socket(client);
            adapter.stop();

            health = adapter.health_snapshot();
            require(!health.listener_present && !health.healthy,
                    "health snapshot should report listener absent and unhealthy after stop");
        }

        {
            auto protocol_service = protocol_services[0];
            protocol_service.host = "127.0.0.1";
            protocol_service.port = reserve_tcp_port();
            protocol_service.handler = "line_echo.throw_on_data";
            require(protocol_service.port != 0, "health snapshot fault-count test should reserve a TCP port");

            yuan::app::PluginProtocolServiceAdapter adapter(plugin_examples_dir.string(), protocol_service);
            adapter.set_runtime_context(context);
            require(adapter.init(), "health snapshot fault-count adapter should initialize");
            adapter.start();
            require(adapter.started(), "health snapshot fault-count adapter should start");

            const socket_t client = connect_tcp_socket(protocol_service.port);
            require(client != kInvalidSocket, "health snapshot fault-count client should connect");
            require(tcp_send(client, "health-fault\n"),
                    "health snapshot fault-count test should send failing payload");
            (void)wait_for_socket_close(client, std::chrono::seconds(2));
            close_socket(client);

            auto health = adapter.health_snapshot();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                if (health.handler_fault_count > 0 && health.total_fault_count >= health.handler_fault_count) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                health = adapter.health_snapshot();
            }

            require(health.listener_present,
                    "health snapshot should report listener present while adapter is running");
            require(health.handler_fault_count > 0,
                    "health snapshot should report handler fault count after handler throw");
            require(health.total_fault_count >= health.handler_fault_count,
                    "health snapshot should aggregate handler faults into total faults");

            adapter.stop();
        }

        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }
    }

    void test_protocol_service_adapter_negative_paths()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-negative-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root);

        write_script_protocol_plugin(
            temp_root,
            "proto_udp_bad_framing",
            {
                {"name", "udp_bad_framing"},
                {"type", "echo"},
                {"transport", "udp"},
                {"host", "127.0.0.1"},
                {"port", 0},
                {"framing", "line"},
                {"contract_id", "plugin.udp.bad_framing"},
            });

        write_script_protocol_plugin(
            temp_root,
            "proto_missing_handler",
            {
                {"name", "custom_proto"},
                {"type", "custom"},
                {"transport", "tcp"},
                {"host", "127.0.0.1"},
                {"port", 0},
                {"handler", "main.on_connection"},
                {"contract_id", "plugin.custom"},
            });

        write_script_protocol_plugin(
            temp_root,
            "proto_udp_missing_handler",
            {
                {"name", "udp_custom_proto"},
                {"type", "custom"},
                {"transport", "udp"},
                {"host", "127.0.0.1"},
                {"port", 0},
                {"handler", "main.on_datagram"},
                {"contract_id", "plugin.udp.custom"},
            });

        write_script_protocol_plugin(
            temp_root,
            "proto_no_runtime",
            {
                {"name", "echo_proto"},
                {"type", "echo"},
                {"protocol", "tcp"},
                {"host", "127.0.0.1"},
                {"port", 0},
                {"contract_id", "plugin.echo.no_runtime"},
            });

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext runtime_context;
        runtime_context.app_name = "protocol-service-negative-path-test";
        runtime_context.run_mode = yuan::app::RunMode::single_thread;
        runtime_context.worker_threads = 1;
        runtime_context.runtime_worker_count = 1;
        runtime_context.runtime_workers.worker_count = 1;
        runtime_context.shared_runtime = &runtime;
        runtime_context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::mutex event_mutex;
        std::vector<yuan::plugin::PluginProtocolServiceEvent> bind_failed_events;
        runtime_context.event_bus->subscribe(
            yuan::plugin::events::plugin_protocol_service_bind_failed,
            [&](const yuan::eventbus::Event &event) {
                const auto *payload = std::any_cast<yuan::plugin::PluginProtocolServiceEvent>(&event.payload);
                if (!payload) {
                    return;
                }
                std::lock_guard<std::mutex> lock(event_mutex);
                bind_failed_events.push_back(*payload);
            });

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        const auto udp_bad_framing_services = discovery_manager.discover_protocol_services({ "proto_udp_bad_framing" });
        require(udp_bad_framing_services.size() == 1, "udp bad-framing service should still be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), udp_bad_framing_services[0]);
            adapter.set_runtime_context(runtime_context);
            require(adapter.init(), "udp bad-framing adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "unsupported udp framing should not start listener");
            adapter.stop();
        }

        const auto missing_handler_services = discovery_manager.discover_protocol_services({ "proto_missing_handler" });
        require(missing_handler_services.size() == 1, "missing handler service should still be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), missing_handler_services[0]);
            adapter.set_runtime_context(runtime_context);
            require(adapter.init(), "missing handler adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "unregistered handler should not start listener");
            adapter.stop();
        }

        const auto udp_missing_handler_services = discovery_manager.discover_protocol_services({ "proto_udp_missing_handler" });
        require(udp_missing_handler_services.size() == 1, "udp missing-handler service should still be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), udp_missing_handler_services[0]);
            adapter.set_runtime_context(runtime_context);
            require(adapter.init(), "udp missing-handler adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "unregistered udp datagram handler should not start listener");
            adapter.stop();
        }

        const auto no_runtime_services = discovery_manager.discover_protocol_services({ "proto_no_runtime" });
        require(no_runtime_services.size() == 1, "runtime-missing echo service should be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), no_runtime_services[0]);
            auto no_runtime_context = runtime_context;
            no_runtime_context.shared_runtime = nullptr;
            adapter.set_runtime_context(no_runtime_context);
            require(adapter.init(), "runtime-missing adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "missing worker runtime should not start listener");
            adapter.stop();
        }

        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(event_mutex);
            require(bind_failed_events.empty(),
                    "non-bind protocol listener failures should not publish bind_failed events");
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_udp_echo_listener()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-udp-echo-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root / "proto_udp_echo");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "udp protocol test should reserve a port");

        {
            std::ofstream manifest(temp_root / "proto_udp_echo" / "plugin.json");
            require(static_cast<bool>(manifest), "udp echo manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_udp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "udp_echo"},
                        {"type", "echo"},
                        {"transport", "udp"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"framing", "raw"},
                        {"contract_id", "plugin.udp.echo"},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(temp_root / "proto_udp_echo" / "main.lua");
            require(static_cast<bool>(script), "udp echo script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_udp_echo" });
        require(protocol_services.size() == 1, "udp echo service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-udp-echo-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "udp echo adapter should initialize");
        adapter.start();
        require(adapter.started(), "udp echo adapter should start listener");

        require(udp_send_and_expect_echo(port, "udp-echo-payload", std::chrono::seconds(2)),
                "udp protocol echo should roundtrip payload");

        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_lua_custom_udp_echo_listener()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "lua custom udp test should reserve a port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-lua-custom-udp-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_lua_custom_udp";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua custom udp manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_udp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "udp_proto"},
                        {"type", "custom"},
                        {"transport", "udp"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"framing", "raw"},
                        {"handler", "main.on_datagram"},
                        {"contract_id", "plugin.lua.custom.udp"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.lua");
            require(static_cast<bool>(script), "lua custom udp script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_datagram(endpoint, peer, data)\n";
            script << "  return endpoint:send_to(peer, data)\n";
            script << "end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_lua_custom_udp" });
        require(protocol_services.size() == 1, "lua custom udp protocol service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-lua-custom-udp-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "lua custom udp adapter should initialize");
        adapter.start();
        require(adapter.started(), "lua custom udp adapter should start listener");

        require(udp_send_and_expect_echo(port, "lua-custom-udp-echo", std::chrono::seconds(2)),
                "lua custom udp protocol listener should echo datagrams");

        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_ts_custom_udp_echo_listener()
    {
        const uint16_t port = reserve_tcp_port();
        require(port != 0, "ts custom udp test should reserve a port");

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-ts-custom-udp-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "proto_ts_custom_udp";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "ts custom udp manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "typescript"},
                {"entry", "main.ts"},
                {"permissions", "register_protocol_service,listen_udp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "udp_proto"},
                        {"type", "custom"},
                        {"transport", "udp"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"framing", "raw"},
                        {"handler", "main.onDatagram"},
                        {"contract_id", "plugin.ts.custom.udp"},
                        {"contract_version", 1},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(plugin_dir / "main.ts");
            require(static_cast<bool>(script), "ts custom udp script should be creatable");
            script << "function on_init(host) {\n";
            script << "    return host != null;\n";
            script << "}\n";
            script << "function onDatagram(endpoint, peer, data) {\n";
            script << "    return endpoint.sendTo(peer, data);\n";
            script << "}\n";
            script << "function on_enable() {}\n";
            script << "function on_disable() { return; }\n";
            script << "function on_health_check() { return true; }\n";
            script << "function on_release() { return; }\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_ts_custom_udp" });
        require(protocol_services.size() == 1, "ts custom udp protocol service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-ts-custom-udp-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "ts custom udp adapter should initialize");
        adapter.start();
        require(adapter.started(), "ts custom udp adapter should start listener");

        require(udp_send_and_expect_echo(port, "ts-custom-udp-echo", std::chrono::seconds(2)),
                "ts custom udp protocol listener should echo datagrams");

        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_protocol_service_udp_peer_idle_cleanup()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-udp-idle-cleanup-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root / "proto_udp_idle_cleanup");

        const uint16_t port = reserve_tcp_port();
        require(port != 0, "udp idle cleanup test should reserve a port");

        {
            std::ofstream manifest(temp_root / "proto_udp_idle_cleanup" / "plugin.json");
            require(static_cast<bool>(manifest), "udp idle cleanup manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
                {"permissions", "register_protocol_service,listen_udp,use_logger,use_network_runtime"},
                {"protocol_services", nlohmann::json::array({
                    {
                        {"name", "udp_idle_cleanup"},
                        {"type", "echo"},
                        {"transport", "udp"},
                        {"host", "127.0.0.1"},
                        {"port", port},
                        {"framing", "raw"},
                        {"idle_timeout_ms", 150},
                        {"contract_id", "plugin.udp.idle_cleanup"},
                    },
                })},
            };
            manifest << doc.dump(2) << "\n";
        }

        {
            std::ofstream script(temp_root / "proto_udp_idle_cleanup" / "main.lua");
            require(static_cast<bool>(script), "udp idle cleanup script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx) return ctx ~= nil end\n";
            script << "function plugin.on_enable() end\n";
            script << "function plugin.on_disable() end\n";
            script << "function plugin.on_health_check() return true end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());
        const auto protocol_services = discovery_manager.discover_protocol_services({ "proto_udp_idle_cleanup" });
        require(protocol_services.size() == 1, "udp idle cleanup service should be discoverable");

        yuan::net::NetworkRuntime runtime;
        yuan::app::RuntimeContext context;
        context.app_name = "protocol-service-udp-idle-cleanup-test";
        context.run_mode = yuan::app::RunMode::single_thread;
        context.worker_threads = 1;
        context.runtime_worker_count = 1;
        context.runtime_workers.worker_count = 1;
        context.shared_runtime = &runtime;
        context.event_bus = std::make_shared<yuan::eventbus::EventBus>();

        std::thread loop_thread([&runtime]() {
            runtime.run();
        });

        yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), protocol_services[0]);
        adapter.set_runtime_context(context);
        require(adapter.init(), "udp idle cleanup adapter should initialize");
        adapter.start();
        require(adapter.started(), "udp idle cleanup adapter should start listener");

        require(udp_send_and_expect_echo(port, "udp-idle-cleanup", std::chrono::seconds(2)),
                "udp idle cleanup test should establish a tracked peer by roundtrip");

        auto report = adapter.resource_leak_report();
        require(report.find("protocol-udp-peer:proto_udp_idle_cleanup.udp_idle_cleanup#") != std::string::npos,
                "resource report should include tracked udp peer state after traffic");

        const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < cleanup_deadline) {
            report = adapter.resource_leak_report();
            if (report.find("protocol-udp-peer:proto_udp_idle_cleanup.udp_idle_cleanup#") == std::string::npos) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(report.find("protocol-udp-peer:proto_udp_idle_cleanup.udp_idle_cleanup#") == std::string::npos,
                "idle cleanup should remove tracked udp peer state after timeout");

        adapter.stop();
        runtime.dispatch([&runtime]() {
            runtime.stop();
        });
        if (loop_thread.joinable()) {
            loop_thread.join();
        }

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_resource_guard_cleanup_on_stop()
    {
        yuan::app::PluginResourceGuard guard;

        int cleanup_count = 0;

        guard.track("cleanup-test", yuan::plugin::PluginResourceType::event_subscription,
                    [&cleanup_count]() { ++cleanup_count; }, "sub1");
        guard.track("cleanup-test", yuan::plugin::PluginResourceType::scheduler_task,
                    [&cleanup_count]() { ++cleanup_count; }, "task1");
        guard.track("cleanup-test", yuan::plugin::PluginResourceType::callback,
                    [&cleanup_count]() { ++cleanup_count; }, "cb1");

        require(guard.tracked_count("cleanup-test") == 3, "should track 3 resources");

        guard.cleanup_plugin("cleanup-test");
        require(cleanup_count == 3, "all 3 cleanup callbacks should have been called");
        require(guard.tracked_count("cleanup-test") == 0, "no resources should remain after cleanup");
    }

    void test_resource_guard_quota_limits()
    {
        yuan::app::PluginResourceGuard guard;

        yuan::plugin::PluginResourceQuota quota;
        quota.max_total_resources = 2;
        quota.max_resources_by_type[yuan::plugin::PluginResourceType::callback] = 1;
        guard.set_quota("quota-test", quota);

        auto id1 = guard.track("quota-test", yuan::plugin::PluginResourceType::callback,
                               []() {}, "cb1");
        auto id2 = guard.track("quota-test", yuan::plugin::PluginResourceType::callback,
                               []() {}, "cb2");
        auto id3 = guard.track("quota-test", yuan::plugin::PluginResourceType::scheduler_task,
                               []() {}, "task1");
        auto id4 = guard.track("quota-test", yuan::plugin::PluginResourceType::event_subscription,
                               []() {}, "sub1");

        require(id1 != 0, "first callback should be tracked under quota");
        require(id2 == 0, "second callback should be rejected by per-type quota");
        require(id3 != 0, "scheduler task should be tracked under total quota");
        require(id4 == 0, "third total resource should be rejected by total quota");
        require(guard.tracked_count("quota-test") == 2, "quota test should track only accepted resources");

        guard.clear_quota("quota-test");
        auto id5 = guard.track("quota-test", yuan::plugin::PluginResourceType::event_subscription,
                               []() {}, "sub2");
        require(id5 != 0, "resource should be accepted after quota is cleared");
        guard.cleanup_plugin("quota-test");
    }

    yuan::plugin::PluginManifest script_binding_manifest(const std::string &name, const std::string &language)
    {
        yuan::plugin::PluginManifest manifest;
        manifest.name = name;
        manifest.plugin_id = name;
        manifest.version = "1.0.0";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;
        manifest.language = language;
        manifest.entry = language == "lua" ? "main.lua" : "main.ts";
        return manifest;
    }

    yuan::plugin::PluginContext script_binding_context(const std::string &plugin_name,
                                                       TestServiceRegistry &registry,
                                                       TestHttpInterceptor &interceptor,
                                                       yuan::app::PluginResourceGuard &guard)
    {
        yuan::plugin::PluginContext context;
        context.app_name = "script-binding-test";
        context.plugin_name = plugin_name;
        context.service_registry = &registry;
        context.http_interceptor = &interceptor;
        context.resource_guard = &guard;
        return context;
    }

    void test_lua_service_registry_and_http_bindings()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-lua-bindings-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root);
        const auto script_path = temp_root / "main.lua";
        {
            std::ofstream script(script_path);
            require(static_cast<bool>(script), "lua binding test script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx)\n";
            script << "  if not ctx.service_registry or not ctx.http_interceptor then return false end\n";
            script << "  if not ctx.http_interceptor:is_available() then return false end\n";
            script << "  if not ctx.service_registry:register('lua.svc', 'contract.lua', 2, 'lua.type', 'value') then return false end\n";
            script << "  if not ctx.service_registry:has('lua.svc') then return false end\n";
            script << "  local desc = ctx.service_registry:describe('lua.svc')\n";
            script << "  if not desc or desc.contract_id ~= 'contract.lua' or desc.contract_version ~= 2 then return false end\n";
            script << "  if #ctx.service_registry:list() ~= 1 then return false end\n";
            script << "  local mid = ctx.http_interceptor:add_middleware(function(path, method) return true end, 'lua-mw')\n";
            script << "  local rid = ctx.http_interceptor:add_route('/lua', function(path, method) end, 'GET')\n";
            script << "  if mid == 0 or rid == 0 then return false end\n";
            script << "  if not ctx.http_interceptor:remove(mid) then return false end\n";
            script << "  return true\n";
            script << "end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        TestServiceRegistry registry;
        TestHttpInterceptor interceptor;
        yuan::app::PluginResourceGuard guard;
        yuan::plugin::LuaScriptPluginAdapter adapter(script_binding_manifest("lua-binding", "lua"));
        require(adapter.load_script(script_path.string()), "lua binding script should load");
        require(adapter.on_init(script_binding_context("lua-binding", registry, interceptor, guard)),
                "lua service/http binding script should initialize");
        require(registry.has_service("lua.svc"), "lua binding should register a service");
        require(interceptor.count() == 1, "lua binding should leave only the route after middleware removal");
        require(guard.tracked_count("lua-binding") == 3,
                "lua binding should track service registration and both http callbacks until cleanup");
        guard.cleanup_plugin("lua-binding");
        require(!registry.has_service("lua.svc"), "resource cleanup should unregister lua services");

        adapter.on_release();
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_ts_service_registry_and_http_bindings()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-ts-bindings-" + std::to_string(static_cast<unsigned long long>(
                                                       std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root);
        const auto script_path = temp_root / "main.ts";
        {
            std::ofstream script(script_path);
            require(static_cast<bool>(script), "ts binding test script should be creatable");
            script << "function on_init(host) {\n";
            script << "  if (!host.serviceRegistry || !host.httpInterceptor) return false;\n";
            script << "  if (!host.httpInterceptor.isAvailable()) return false;\n";
            script << "  if (!host.serviceRegistry.register('ts.svc', 'contract.ts', 3, 'ts.type', 'value')) return false;\n";
            script << "  if (!host.serviceRegistry.has('ts.svc')) return false;\n";
            script << "  const desc = host.serviceRegistry.describe('ts.svc');\n";
            script << "  if (!desc || desc.contractId !== 'contract.ts' || desc.contractVersion !== 3) return false;\n";
            script << "  if (host.serviceRegistry.list().length !== 1) return false;\n";
            script << "  const mid = host.httpInterceptor.addMiddleware(function(path, method) { return true; }, 'ts-mw');\n";
            script << "  const rid = host.httpInterceptor.addRoute('/ts', function(path, method) {}, 'POST');\n";
            script << "  if (mid === 0 || rid === 0) return false;\n";
            script << "  if (!host.httpInterceptor.remove(mid)) return false;\n";
            script << "  return true;\n";
            script << "}\n";
            script << "function on_release() {}\n";
        }

        TestServiceRegistry registry;
        TestHttpInterceptor interceptor;
        yuan::app::PluginResourceGuard guard;
        yuan::plugin::TsScriptPluginAdapter adapter(script_binding_manifest("ts-binding", "typescript"));
        require(adapter.load_script(script_path.string()), "ts binding script should load");
        require(adapter.on_init(script_binding_context("ts-binding", registry, interceptor, guard)),
                "ts service/http binding script should initialize");
        require(registry.has_service("ts.svc"), "ts binding should register a service");
        require(interceptor.count() == 1, "ts binding should leave only the route after middleware removal");
        require(guard.tracked_count("ts-binding") == 3,
                "ts binding should track service registration and both http callbacks until cleanup");
        guard.cleanup_plugin("ts-binding");
        require(!registry.has_service("ts.svc"), "resource cleanup should unregister ts services");

        adapter.on_release();
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_plugin_host_reload_changed_script_plugins()
    {
        fake_script_load_counts.clear();
        fake_script_cleanup_count = 0;

        const std::string language = "governance-reload-script";
        yuan::plugin::ScriptPluginRegistry::instance().register_adapter(
            language,
            [](const yuan::plugin::PluginManifest &manifest, const yuan::plugin::PluginConfigView &) -> yuan::plugin::ScriptPluginAdapter * {
                return new FakeScriptPlugin(manifest);
            });

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-host-reload-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "reload_script";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "reload script manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", language},
                {"entry", "main.fake"},
            };
            manifest << doc.dump(2) << "\n";
        }
        {
            std::ofstream script(plugin_dir / "main.fake");
            require(static_cast<bool>(script), "reload script entry should be creatable");
            script << "version=1\n";
        }

        yuan::app::PluginHostService host(temp_root.string(), { "reload_script" });
        require(host.init(), "plugin host should load fake reload script");
        require(fake_script_load_counts["reload_script"] == 1, "fake script should load once during host init");

        auto reloaded = host.reload_changed_script_plugins();
        require(reloaded.empty(), "unchanged script scan should not reload plugins");
        require(fake_script_load_counts["reload_script"] == 1, "unchanged script should not load again");

        const auto script_path = plugin_dir / "main.fake";
        std::error_code ec;
        const auto initial_write_time = std::filesystem::last_write_time(script_path, ec);
        require(!ec, "reload script timestamp should be readable");
        std::filesystem::last_write_time(script_path, initial_write_time + std::chrono::seconds(2), ec);
        require(!ec, "reload script timestamp should be updateable");

        reloaded = host.reload_changed_script_plugins();
        require(reloaded.size() == 1 && reloaded[0] == "reload_script",
                "changed script scan should reload the modified plugin");
        require(fake_script_load_counts["reload_script"] == 2, "changed script should load a second time");
        require(fake_script_cleanup_count == 1, "reload should clean up resources from the old plugin instance");
        require(host.health_check_all().size() == 1, "reload should leave exactly one loaded plugin instance");

        host.stop();
        require(fake_script_cleanup_count == 2, "host stop should clean up resources from the reloaded plugin");
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_plugin_manifest_resource_quota_limits_host_resources()
    {
        fake_script_load_counts.clear();
        fake_script_cleanup_count = 0;

        const std::string language = "governance-quota-script";
        yuan::plugin::ScriptPluginRegistry::instance().register_adapter(
            language,
            [](const yuan::plugin::PluginManifest &manifest, const yuan::plugin::PluginConfigView &) -> yuan::plugin::ScriptPluginAdapter * {
                return new FakeScriptPlugin(manifest);
            });

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-host-quota-" + std::to_string(static_cast<unsigned long long>(
                                                       std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto plugin_dir = temp_root / "quota_script";
        std::filesystem::create_directories(plugin_dir);

        {
            std::ofstream manifest(plugin_dir / "plugin.json");
            require(static_cast<bool>(manifest), "quota script manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", language},
                {"entry", "main.fake"},
                {"resource_quota", {
                    {"max_total_resources", 0},
                    {"max_resources_by_type", {
                        {"callback", 0}
                    }}
                }},
            };
            manifest << doc.dump(2) << "\n";
        }
        {
            std::ofstream script(plugin_dir / "main.fake");
            require(static_cast<bool>(script), "quota script entry should be creatable");
            script << "version=1\n";
        }

        yuan::app::PluginHostService host(temp_root.string(), { "quota_script" });
        require(host.init(), "plugin host should load fake quota script");
        require(fake_script_load_counts["quota_script"] == 1, "quota script should load once");

        host.stop();
        require(fake_script_cleanup_count == 0,
                "manifest resource_quota should reject the fake script tracked callback resource");

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_script_bindings_respect_manifest_permissions()
    {
        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-permission-bindings-" + std::to_string(static_cast<unsigned long long>(
                                                           std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto lua_dir = temp_root / "lua_no_caps";
        const auto ts_dir = temp_root / "ts_no_caps";
        std::filesystem::create_directories(lua_dir);
        std::filesystem::create_directories(ts_dir);

        {
            std::ofstream manifest(lua_dir / "plugin.json");
            require(static_cast<bool>(manifest), "lua no-caps manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "lua"},
                {"entry", "main.lua"},
            };
            manifest << doc.dump(2) << "\n";
        }
        {
            std::ofstream script(lua_dir / "main.lua");
            require(static_cast<bool>(script), "lua no-caps script should be creatable");
            script << "local plugin = {}\n";
            script << "function plugin.on_init(ctx)\n";
            script << "  return ctx.service_registry == nil and ctx.http_interceptor == nil\n";
            script << "end\n";
            script << "function plugin.on_release() end\n";
            script << "return plugin\n";
        }

        {
            std::ofstream manifest(ts_dir / "plugin.json");
            require(static_cast<bool>(manifest), "ts no-caps manifest should be creatable");
            const auto doc = nlohmann::json{
                {"run_mode", "script"},
                {"language", "typescript"},
                {"entry", "main.ts"},
            };
            manifest << doc.dump(2) << "\n";
        }
        {
            std::ofstream script(ts_dir / "main.ts");
            require(static_cast<bool>(script), "ts no-caps script should be creatable");
            script << "function on_init(host) {\n";
            script << "  return host.serviceRegistry === undefined && host.httpInterceptor === undefined;\n";
            script << "}\n";
            script << "function on_release() {}\n";
        }

        yuan::app::PluginHostService host(temp_root.string(), { "lua_no_caps", "ts_no_caps" });
        require(host.init(), "scripts without service/http permissions should initialize with stripped bindings");
        require(host.health_check_all().size() == 2, "permission binding test should load both scripts");
        host.stop();

        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

    void test_lifecycle_manager_cleanup_on_stop()
    {
        yuan::app::PluginResourceGuard guard;
        yuan::app::PluginServiceRegistryAdapter registry;

        yuan::plugin::PluginLifecycleManager mgr;
        mgr.set_resource_guard(&guard);
        mgr.set_service_registry(&registry);

        int cleanup_count = 0;
        guard.track("lm-cleanup", yuan::plugin::PluginResourceType::event_subscription,
                    [&cleanup_count]() { ++cleanup_count; }, "sub1");

        auto plugin = new StubPlugin();
        mgr.register_instance("lm-cleanup", plugin, nullptr);
        mgr.transition("lm-cleanup", yuan::plugin::PluginState::initialized);
        mgr.transition("lm-cleanup", yuan::plugin::PluginState::active);

        mgr.stop("lm-cleanup");

        require(mgr.state("lm-cleanup") == yuan::plugin::PluginState::stopped,
                "plugin should be stopped after stop()");
        require(cleanup_count == 1, "resources should be cleaned up when plugin stops");
        require(!guard.has_tracked_resources("lm-cleanup"), "no resources should remain after stop");

        mgr.unload("lm-cleanup");
    }

    void test_load_all_rejects_missing_dependencies()
    {
        auto plugin_manager = yuan::plugin::PluginManager::get_instance();
        plugin_manager->release_all();

        const std::string language = "governance-test-script";
        yuan::plugin::ScriptPluginRegistry::instance().register_adapter(
            language,
            [](const yuan::plugin::PluginManifest &manifest, const yuan::plugin::PluginConfigView &) -> yuan::plugin::ScriptPluginAdapter * {
                return new FakeScriptPlugin(manifest);
            });

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-load-all-" + std::to_string(static_cast<unsigned long long>(
                                                        std::chrono::steady_clock::now().time_since_epoch().count())));
        std::filesystem::create_directories(temp_root);

        const auto manifest_path = temp_root / "orphan.json";
        std::ofstream manifest(manifest_path);
        require(static_cast<bool>(manifest), "temporary manifest file should be creatable");
        const auto doc = nlohmann::json{
            {"run_mode", "script"},
            {"language", language},
            {"entry", "main.lua"},
            {"depends_on", nlohmann::json::array({ "missing_dep" })},
        };
        manifest << doc.dump(2) << "\n";
        manifest.close();

        plugin_manager->set_plugin_path(temp_root.string());
        plugin_manager->set_context(yuan::plugin::PluginContext{});

        const bool loaded = plugin_manager->load_all({ "orphan" });
        require(!loaded, "load_all should reject plugins with missing dependencies");
        require(plugin_manager->loaded_plugin_names().empty(),
                "load_all should not leave partially loaded plugins behind");

        plugin_manager->release_all();
        std::error_code ec;
        std::filesystem::remove_all(temp_root, ec);
    }

} // namespace

int main()
{
    std::cout << "=== Phase C: State Machine Tests ===" << std::endl;
    test_state_machine_transitions();
    test_operational_and_callback_states();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Fault Injection Tests ===" << std::endl;
    test_call_guard_fault_accumulation();
    test_call_guard_blocks_faulted_plugin();
    test_call_guard_fault_handler();
    test_call_guard_custom_thresholds();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Lifecycle Manager Tests ===" << std::endl;
    test_lifecycle_manager_state_transitions();
    test_lifecycle_manager_stop_from_loaded_state();
    test_lifecycle_manager_fault_escalation();
    test_lifecycle_manager_state_change_callback();
    test_lifecycle_manager_cleanup_on_stop();
    test_load_all_rejects_missing_dependencies();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Descriptor Compatibility Tests ===" << std::endl;
    test_manifest_from_meta();
    test_event_descriptors();
    test_protocol_service_lifecycle_events();
    test_protocol_connection_events();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Capability Enforcement Tests ===" << std::endl;
    test_capability_enforcement();
    test_plugin_context_identity_boundary();
    test_protocol_service_permission_names();
    test_protocol_handler_registry_datagram_groundwork();
    test_protocol_service_manifest_discovery();
    test_protocol_service_worker_local_adapter();
    test_protocol_service_multi_worker_reuse_port_identity_isolation();
    test_protocol_service_echo_listener();
    test_protocol_service_lua_custom_line_echo_listener();
    test_protocol_service_lua_handler_returns_false();
    test_protocol_service_lua_handler_error();
    test_protocol_service_lua_idle_timeout_closes_partial_line_connections();
    test_protocol_service_lua_example_line_echo_listener();
    test_protocol_service_ts_example_line_echo_listener();
    test_protocol_service_cpp_line_echo_listener();
    test_protocol_service_handler_fault_events();
    test_protocol_service_shutdown_closes_active_connections();
    test_protocol_service_resource_guard_tracking();
    test_protocol_service_idle_timeout_closes_partial_line_connections();
    test_protocol_service_length_prefixed_framing();
    test_protocol_service_runtime_error_stats_split();
    test_protocol_service_backpressure_write_buffer_limit();
    test_protocol_service_health_snapshot();
    test_protocol_service_udp_echo_listener();
    test_protocol_service_lua_custom_udp_echo_listener();
    test_protocol_service_ts_custom_udp_echo_listener();
    test_protocol_service_udp_peer_idle_cleanup();
    test_protocol_service_adapter_negative_paths();
    test_resource_guard_cleanup_on_stop();
    test_resource_guard_quota_limits();
    test_lua_service_registry_and_http_bindings();
    test_ts_service_registry_and_http_bindings();
    test_plugin_host_reload_changed_script_plugins();
    test_plugin_manifest_resource_quota_limits_host_resources();
    test_script_bindings_respect_manifest_permissions();
    std::cout << "  PASSED" << std::endl;

    std::cout << "all phase C governance tests passed" << std::endl;
    return 0;
}
