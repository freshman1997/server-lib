#include "bootstrap.h"
#include "eventbus/event_bus.h"
#include "plugin_resource_guard.h"
#include "plugin_protocol_service_adapter.h"
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
#include "net/runtime/network_runtime.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

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
                                      const std::string &protocol_service_json)
    {
        const auto plugin_dir = root / plugin_name;
        std::filesystem::create_directories(plugin_dir);

        std::ofstream manifest(plugin_dir / "plugin.json");
        require(static_cast<bool>(manifest), "protocol plugin manifest should be creatable");
        manifest << "{\n";
        manifest << "  \"run_mode\": \"script\",\n";
        manifest << "  \"language\": \"lua\",\n";
        manifest << "  \"entry\": \"main.lua\",\n";
        manifest << "  \"permissions\": \"register_protocol_service,use_logger,use_network_runtime\",\n";
        manifest << "  \"protocol_services\": [\n";
        manifest << protocol_service_json << "\n";
        manifest << "  ]\n";
        manifest << "}\n";
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
            return true;
        }

    protected:
        bool do_init(const yuan::plugin::PluginContext &) override
        {
            return true;
        }
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
            "register_protocol_service,use_network_runtime");
        require(yuan::plugin::has_permission(
                    permissions,
                    yuan::plugin::PluginPermission::register_protocol_service),
                "register_protocol_service permission should parse");
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
    }

    void test_protocol_service_manifest_discovery()
    {
        yuan::plugin::PluginManager manager;

        const auto temp_root = std::filesystem::temp_directory_path() /
                               ("plugin-protocol-services-" + std::to_string(static_cast<unsigned long long>(
                                                                std::chrono::steady_clock::now().time_since_epoch().count())));
        const auto allowed_dir = temp_root / "proto_allowed";
        const auto denied_dir = temp_root / "proto_denied";
        const auto invalid_dir = temp_root / "proto_invalid";
        std::filesystem::create_directories(allowed_dir);
        std::filesystem::create_directories(denied_dir);
        std::filesystem::create_directories(invalid_dir);

        {
            std::ofstream manifest(allowed_dir / "plugin.json");
            require(static_cast<bool>(manifest), "allowed protocol service manifest should be creatable");
            manifest << "{\n";
            manifest << "  \"run_mode\": \"script\",\n";
            manifest << "  \"language\": \"lua\",\n";
            manifest << "  \"entry\": \"main.lua\",\n";
            manifest << "  \"permissions\": \"register_protocol_service,use_network_runtime\",\n";
            manifest << "  \"protocol_services\": [{\n";
            manifest << "    \"name\": \"echo_proto\",\n";
            manifest << "    \"type\": \"custom\",\n";
            manifest << "    \"protocol\": \"tcp\",\n";
            manifest << "    \"transport\": \"tcp\",\n";
            manifest << "    \"host\": \"127.0.0.1\",\n";
            manifest << "    \"port\": 19090,\n";
            manifest << "    \"handler\": \"main.on_connection\",\n";
            manifest << "    \"framing\": \"line\",\n";
            manifest << "    \"read_timeout_ms\": 31000,\n";
            manifest << "    \"idle_timeout_ms\": 62000,\n";
            manifest << "    \"write_timeout_ms\": 32000,\n";
            manifest << "    \"max_connections\": 2048,\n";
            manifest << "    \"max_frame_bytes\": 32768,\n";
            manifest << "    \"contract_id\": \"plugin.echo\",\n";
            manifest << "    \"contract_version\": 2\n";
            manifest << "  }]\n";
            manifest << "}\n";
        }

        {
            std::ofstream manifest(denied_dir / "plugin.json");
            require(static_cast<bool>(manifest), "denied protocol service manifest should be creatable");
            manifest << "{\n";
            manifest << "  \"run_mode\": \"script\",\n";
            manifest << "  \"language\": \"lua\",\n";
            manifest << "  \"permissions\": \"use_network_runtime\",\n";
            manifest << "  \"protocol_services\": [{\n";
            manifest << "    \"name\": \"blocked_proto\",\n";
            manifest << "    \"protocol\": \"tcp\",\n";
            manifest << "    \"port\": 19091,\n";
            manifest << "    \"contract_id\": \"plugin.blocked\"\n";
            manifest << "  }]\n";
            manifest << "}\n";
        }

        {
            std::ofstream manifest(invalid_dir / "plugin.json");
            require(static_cast<bool>(manifest), "invalid protocol service manifest should be creatable");
            manifest << "{\n";
            manifest << "  \"run_mode\": \"script\",\n";
            manifest << "  \"language\": \"lua\",\n";
            manifest << "  \"permissions\": \"register_protocol_service,use_network_runtime\",\n";
            manifest << "  \"protocol_services\": [{\n";
            manifest << "    \"name\": \"bad_proto\",\n";
            manifest << "    \"transport\": \"tcp\",\n";
            manifest << "    \"port\": 70000,\n";
            manifest << "    \"contract_id\": \"plugin.bad\"\n";
            manifest << "  }]\n";
            manifest << "}\n";
        }

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
        require(allowed[0].contract_id == "plugin.echo", "protocol service should carry contract id");
        require(allowed[0].contract_version == 2, "protocol service should carry contract version");
        require(allowed[0].run_mode == yuan::plugin::PluginRunMode::script,
                "protocol service should carry plugin run mode");
        require(allowed[0].language == "lua", "protocol service should carry language");
        require(allowed[0].entry == "main.lua", "protocol service should carry entry");

        const auto denied = manager.discover_protocol_services({ "proto_denied" });
        require(denied.empty(), "plugin manager should reject protocol services without permission");

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
            manifest << "{\n";
            manifest << "  \"run_mode\": \"script\",\n";
            manifest << "  \"language\": \"lua\",\n";
            manifest << "  \"entry\": \"main.lua\",\n";
            manifest << "  \"permissions\": \"register_protocol_service,use_logger,use_network_runtime\",\n";
            manifest << "  \"protocol_services\": [{\n";
            manifest << "    \"name\": \"echo_proto\",\n";
            manifest << "    \"type\": \"echo\",\n";
            manifest << "    \"protocol\": \"tcp\",\n";
            manifest << "    \"host\": \"127.0.0.1\",\n";
            manifest << "    \"port\": 0,\n";
            manifest << "    \"contract_id\": \"plugin.echo.worker\",\n";
            manifest << "    \"contract_version\": 1\n";
            manifest << "  }]\n";
            manifest << "}\n";
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
            manifest << "{\n";
            manifest << "  \"run_mode\": \"script\",\n";
            manifest << "  \"language\": \"lua\",\n";
            manifest << "  \"entry\": \"main.lua\",\n";
            manifest << "  \"permissions\": \"register_protocol_service,use_logger,use_network_runtime\",\n";
            manifest << "  \"protocol_services\": [{\n";
            manifest << "    \"name\": \"echo_proto\",\n";
            manifest << "    \"type\": \"echo\",\n";
            manifest << "    \"protocol\": \"tcp\",\n";
            manifest << "    \"host\": \"127.0.0.1\",\n";
            manifest << "    \"port\": " << port << ",\n";
            manifest << "    \"contract_id\": \"plugin.echo.listener\",\n";
            manifest << "    \"contract_version\": 1\n";
            manifest << "  }]\n";
            manifest << "}\n";
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

            close_socket(client);
            adapter.stop();
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
            require(wait_for_socket_close(client, std::chrono::seconds(2)),
                    "framing-error stats test connection should be closed");

            auto stats = adapter.runtime_stats();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (stats.framing_error_count == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                stats = adapter.runtime_stats();
            }

            require(stats.framing_error_count > 0,
                    "framing failures should increment the framing error count");
            require(stats.handler_error_count == 0,
                    "framing failures should not increment the handler error count");

            close_socket(client);
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
            "proto_udp",
            "    {\n"
            "      \"name\": \"udp_proto\",\n"
            "      \"type\": \"echo\",\n"
            "      \"transport\": \"udp\",\n"
            "      \"host\": \"127.0.0.1\",\n"
            "      \"port\": 0,\n"
            "      \"contract_id\": \"plugin.udp\"\n"
            "    }");

        write_script_protocol_plugin(
            temp_root,
            "proto_missing_handler",
            "    {\n"
            "      \"name\": \"custom_proto\",\n"
            "      \"type\": \"custom\",\n"
            "      \"transport\": \"tcp\",\n"
            "      \"host\": \"127.0.0.1\",\n"
            "      \"port\": 0,\n"
            "      \"handler\": \"main.on_connection\",\n"
            "      \"contract_id\": \"plugin.custom\"\n"
            "    }");

        write_script_protocol_plugin(
            temp_root,
            "proto_no_runtime",
            "    {\n"
            "      \"name\": \"echo_proto\",\n"
            "      \"type\": \"echo\",\n"
            "      \"protocol\": \"tcp\",\n"
            "      \"host\": \"127.0.0.1\",\n"
            "      \"port\": 0,\n"
            "      \"contract_id\": \"plugin.echo.no_runtime\"\n"
            "    }");

        yuan::plugin::PluginManager discovery_manager;
        discovery_manager.set_plugin_path(temp_root.string());

        const auto udp_services = discovery_manager.discover_protocol_services({ "proto_udp" });
        require(udp_services.size() == 1, "unsupported transport service should still be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), udp_services[0]);
            require(adapter.init(), "unsupported transport adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "unsupported transport should not start listener");
            adapter.stop();
        }

        const auto missing_handler_services = discovery_manager.discover_protocol_services({ "proto_missing_handler" });
        require(missing_handler_services.size() == 1, "missing handler service should still be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), missing_handler_services[0]);
            require(adapter.init(), "missing handler adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "unregistered handler should not start listener");
            adapter.stop();
        }

        const auto no_runtime_services = discovery_manager.discover_protocol_services({ "proto_no_runtime" });
        require(no_runtime_services.size() == 1, "runtime-missing echo service should be discoverable");
        {
            yuan::app::PluginProtocolServiceAdapter adapter(temp_root.string(), no_runtime_services[0]);
            require(adapter.init(), "runtime-missing adapter should initialize plugin runtime");
            adapter.start();
            require(!adapter.started(), "missing worker runtime should not start listener");
            adapter.stop();
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
        manifest << "{\n";
        manifest << "  \"run_mode\": \"script\",\n";
        manifest << "  \"language\": \"" << language << "\",\n";
        manifest << "  \"entry\": \"main.lua\",\n";
        manifest << "  \"depends_on\": [\"missing_dep\"]\n";
        manifest << "}\n";
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
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Phase C: Capability Enforcement Tests ===" << std::endl;
    test_capability_enforcement();
    test_plugin_context_identity_boundary();
    test_protocol_service_permission_names();
    test_protocol_service_manifest_discovery();
    test_protocol_service_worker_local_adapter();
    test_protocol_service_echo_listener();
    test_protocol_service_cpp_line_echo_listener();
    test_protocol_service_handler_fault_events();
    test_protocol_service_shutdown_closes_active_connections();
    test_protocol_service_resource_guard_tracking();
    test_protocol_service_idle_timeout_closes_partial_line_connections();
    test_protocol_service_length_prefixed_framing();
    test_protocol_service_runtime_error_stats_split();
    test_protocol_service_adapter_negative_paths();
    test_resource_guard_cleanup_on_stop();
    std::cout << "  PASSED" << std::endl;

    std::cout << "all phase C governance tests passed" << std::endl;
    return 0;
}
