#ifndef __NET_CONNECTOR_HANDLER_H__
#define __NET_CONNECTOR_HANDLER_H__

#include <memory>
#include <cstdint>

namespace yuan::net 
{
    class Connection;

    enum class ConnectResultCode
    {
        success,
        failed,
        timeout,
    };

    struct ConnectResult
    {
        ConnectResultCode code = ConnectResultCode::failed;
        std::shared_ptr<Connection> connection;
        int error_code = 0;
        uint64_t attempt_id = 0;
    };

    class ConnectorHandler
    {
    public:
        virtual ~ConnectorHandler() = default;

        virtual void on_connect_result(const ConnectResult &result) = 0;
    };
}

#endif
