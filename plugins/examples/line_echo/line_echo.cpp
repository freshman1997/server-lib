#include "api/api.h"
#include "plugin/plugin.h"
#include "plugin/plugin_permission.h"
#include "plugin/plugin_protocol_handler.h"

#include <array>
#include <memory>
#include <span>
#include <stdexcept>

namespace
{
    class LineEchoHandler final : public yuan::plugin::PluginStreamProtocolHandler
    {
    public:
        bool on_data(yuan::plugin::HostStreamConnection &connection,
                     std::span<const std::byte> bytes) override
        {
            return connection.write(bytes) &&
                   connection.write("\n") &&
                   connection.flush();
        }
    };

    class RejectingLineEchoHandler final : public yuan::plugin::PluginStreamProtocolHandler
    {
    public:
        bool on_data(yuan::plugin::HostStreamConnection &,
                     std::span<const std::byte>) override
        {
            return false;
        }
    };

    class ThrowingLineEchoHandler final : public yuan::plugin::PluginStreamProtocolHandler
    {
    public:
        bool on_data(yuan::plugin::HostStreamConnection &,
                     std::span<const std::byte>) override
        {
            throw std::runtime_error("line echo handler test failure");
        }
    };

    class LengthPrefixedEchoHandler final : public yuan::plugin::PluginStreamProtocolHandler
    {
    public:
        bool on_data(yuan::plugin::HostStreamConnection &connection,
                     std::span<const std::byte> bytes) override
        {
            const auto frame_size = static_cast<std::uint32_t>(bytes.size());
            const std::array<std::byte, 4> header{
                std::byte{ static_cast<unsigned char>((frame_size >> 24u) & 0xffu) },
                std::byte{ static_cast<unsigned char>((frame_size >> 16u) & 0xffu) },
                std::byte{ static_cast<unsigned char>((frame_size >> 8u) & 0xffu) },
                std::byte{ static_cast<unsigned char>(frame_size & 0xffu) }
            };
            return connection.write(std::span<const std::byte>(header.data(), header.size())) &&
                   connection.write(bytes) &&
                   connection.flush();
        }
    };

    class LineEchoProtocolPlugin final : public yuan::plugin::Plugin
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

        yuan::plugin::PluginMeta meta() const override
        {
            yuan::plugin::PluginMeta meta;
            meta.name = "LineEchoProtocol";
            meta.version = "1.0.0";
            meta.author = "yuan";
            meta.description = "C++ demo plugin for custom TCP line protocol handlers";
            meta.api_version = 1;
            meta.required_permissions =
                yuan::plugin::PluginPermission::register_protocol_service |
                yuan::plugin::PluginPermission::use_network_runtime;
            return meta;
        }

        void register_protocol_handlers(yuan::plugin::PluginProtocolHandlerRegistry &registry) override
        {
            registry.register_stream_handler(
                "line_echo.on_connection",
                [](const yuan::plugin::ProtocolServiceDescriptor &) {
                    return std::make_unique<LineEchoHandler>();
                });
            registry.register_stream_handler(
                "line_echo.reject_on_data",
                [](const yuan::plugin::ProtocolServiceDescriptor &) {
                    return std::make_unique<RejectingLineEchoHandler>();
                });
            registry.register_stream_handler(
                "line_echo.throw_on_data",
                [](const yuan::plugin::ProtocolServiceDescriptor &) {
                    return std::make_unique<ThrowingLineEchoHandler>();
                });
            registry.register_stream_handler(
                "length_echo.on_connection",
                [](const yuan::plugin::ProtocolServiceDescriptor &) {
                    return std::make_unique<LengthPrefixedEchoHandler>();
                });
        }
    };
} // namespace

YUAN_API_C_EXPORT void *get_LineEchoProtocol_plugin_instance()
{
    return new LineEchoProtocolPlugin;
}
