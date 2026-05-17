#include "api/api.h"
#include "plugin/plugin.h"
#include "plugin/plugin_permission.h"
#include "plugin/plugin_protocol_handler.h"

#include <memory>
#include <span>

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
        }
    };
} // namespace

YUAN_API_C_EXPORT void *get_LineEchoProtocol_plugin_instance()
{
    return new LineEchoProtocolPlugin;
}
