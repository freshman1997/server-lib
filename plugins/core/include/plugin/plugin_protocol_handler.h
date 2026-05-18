#ifndef __YUAN_PLUGIN_PLUGIN_PROTOCOL_HANDLER_H__
#define __YUAN_PLUGIN_PLUGIN_PROTOCOL_HANDLER_H__

#include "plugin/plugin_manifest.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace yuan::plugin
{

    struct ProtocolHandlerErrorInfo
    {
        int code = 0;
        std::string message;
    };

    class HostStreamConnection
    {
    public:
        virtual ~HostStreamConnection() = default;

        virtual std::uintptr_t id() const = 0;
        virtual std::string peer_address() const = 0;
        virtual std::string local_address() const = 0;
        virtual bool write(std::span<const std::byte> bytes) = 0;
        virtual bool flush() = 0;
        virtual void close() = 0;
        virtual bool is_open() const = 0;

        bool write(std::string_view text)
        {
            return write(std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(text.data()),
                text.size()));
        }
    };

    class HostDatagramEndpoint
    {
    public:
        virtual ~HostDatagramEndpoint() = default;

        virtual std::string local_address() const = 0;
        virtual bool send_to(std::string_view peer, std::span<const std::byte> bytes) = 0;

        bool send_to(std::string_view peer, std::string_view text)
        {
            return send_to(peer,
                           std::span<const std::byte>(
                               reinterpret_cast<const std::byte *>(text.data()),
                               text.size()));
        }
    };

    class PluginStreamProtocolHandler
    {
    public:
        virtual ~PluginStreamProtocolHandler() = default;

        virtual bool on_accept(HostStreamConnection &connection)
        {
            (void)connection;
            return true;
        }

        virtual bool on_data(HostStreamConnection &connection, std::span<const std::byte> bytes) = 0;

        virtual void on_close(HostStreamConnection &connection)
        {
            (void)connection;
        }

        virtual void on_error(HostStreamConnection &connection, const ProtocolHandlerErrorInfo &error)
        {
            (void)connection;
            (void)error;
        }
    };

    using PluginStreamProtocolHandlerFactory =
        std::function<std::unique_ptr<PluginStreamProtocolHandler>(const ProtocolServiceDescriptor &)>;

    class PluginDatagramProtocolHandler
    {
    public:
        virtual ~PluginDatagramProtocolHandler() = default;

        virtual bool on_datagram(HostDatagramEndpoint &endpoint,
                                 std::string_view peer,
                                 std::span<const std::byte> bytes) = 0;

        virtual void on_error(HostDatagramEndpoint &endpoint,
                              std::string_view peer,
                              const ProtocolHandlerErrorInfo &error)
        {
            (void)endpoint;
            (void)peer;
            (void)error;
        }
    };

    using PluginDatagramProtocolHandlerFactory =
        std::function<std::unique_ptr<PluginDatagramProtocolHandler>(const ProtocolServiceDescriptor &)>;

    class PluginProtocolHandlerRegistry
    {
    public:
        bool register_stream_handler(std::string name, PluginStreamProtocolHandlerFactory factory);
        bool has_stream_handler(const std::string &name) const;
        PluginStreamProtocolHandlerFactory find_stream_handler(const std::string &name) const;
        bool register_datagram_handler(std::string name, PluginDatagramProtocolHandlerFactory factory);
        bool has_datagram_handler(const std::string &name) const;
        PluginDatagramProtocolHandlerFactory find_datagram_handler(const std::string &name) const;
        std::size_t stream_handler_count() const;
        std::size_t datagram_handler_count() const;
        void clear();

    private:
        std::unordered_map<std::string, PluginStreamProtocolHandlerFactory> stream_handlers_;
        std::unordered_map<std::string, PluginDatagramProtocolHandlerFactory> datagram_handlers_;
    };

    void register_builtin_protocol_handlers(PluginProtocolHandlerRegistry &registry);

} // namespace yuan::plugin

#endif
