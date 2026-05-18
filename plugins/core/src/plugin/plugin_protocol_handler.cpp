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

        class BuiltinEchoDatagramProtocolHandler final : public PluginDatagramProtocolHandler
        {
        public:
            bool on_datagram(HostDatagramEndpoint &endpoint,
                             std::string_view peer,
                             std::span<const std::byte> bytes) override
            {
                return endpoint.send_to(peer, bytes);
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

    bool PluginProtocolHandlerRegistry::register_datagram_handler(
        std::string name,
        PluginDatagramProtocolHandlerFactory factory)
    {
        if (name.empty() || !factory) {
            return false;
        }

        datagram_handlers_[std::move(name)] = std::move(factory);
        return true;
    }

    bool PluginProtocolHandlerRegistry::has_datagram_handler(const std::string &name) const
    {
        return datagram_handlers_.find(name) != datagram_handlers_.end();
    }

    PluginDatagramProtocolHandlerFactory PluginProtocolHandlerRegistry::find_datagram_handler(
        const std::string &name) const
    {
        const auto it = datagram_handlers_.find(name);
        return it == datagram_handlers_.end() ? PluginDatagramProtocolHandlerFactory{} : it->second;
    }

    std::size_t PluginProtocolHandlerRegistry::stream_handler_count() const
    {
        return stream_handlers_.size();
    }

    std::size_t PluginProtocolHandlerRegistry::datagram_handler_count() const
    {
        return datagram_handlers_.size();
    }

    void PluginProtocolHandlerRegistry::clear()
    {
        stream_handlers_.clear();
        datagram_handlers_.clear();
    }

    void register_builtin_protocol_handlers(PluginProtocolHandlerRegistry &registry)
    {
        registry.register_stream_handler(
            "builtin.echo",
            [](const ProtocolServiceDescriptor &) {
                return std::make_unique<BuiltinEchoProtocolHandler>();
            });
        registry.register_datagram_handler(
            "builtin.echo",
            [](const ProtocolServiceDescriptor &) {
                return std::make_unique<BuiltinEchoDatagramProtocolHandler>();
            });
    }

} // namespace yuan::plugin
