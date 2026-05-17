#include "plugin/plugin_protocol_handler.h"

#include <utility>

namespace yuan::plugin
{
    namespace
    {
        class BuiltinEchoProtocolHandler final : public PluginStreamProtocolHandler
        {
        public:
            bool on_data(HostStreamConnection &connection, std::span<const std::byte> bytes) override
            {
                return connection.write(bytes) && connection.flush();
            }
        };
    } // namespace

    bool PluginProtocolHandlerRegistry::register_stream_handler(
        std::string name,
        PluginStreamProtocolHandlerFactory factory)
    {
        if (name.empty() || !factory) {
            return false;
        }

        stream_handlers_[std::move(name)] = std::move(factory);
        return true;
    }

    bool PluginProtocolHandlerRegistry::has_stream_handler(const std::string &name) const
    {
        return stream_handlers_.find(name) != stream_handlers_.end();
    }

    PluginStreamProtocolHandlerFactory PluginProtocolHandlerRegistry::find_stream_handler(
        const std::string &name) const
    {
        const auto it = stream_handlers_.find(name);
        return it == stream_handlers_.end() ? PluginStreamProtocolHandlerFactory{} : it->second;
    }

    std::size_t PluginProtocolHandlerRegistry::stream_handler_count() const
    {
        return stream_handlers_.size();
    }

    void PluginProtocolHandlerRegistry::clear()
    {
        stream_handlers_.clear();
    }

    void register_builtin_protocol_handlers(PluginProtocolHandlerRegistry &registry)
    {
        registry.register_stream_handler(
            "builtin.echo",
            [](const ProtocolServiceDescriptor &) {
                return std::make_unique<BuiltinEchoProtocolHandler>();
            });
    }

} // namespace yuan::plugin
