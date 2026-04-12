#ifndef __TCP_SOCKET_HANDLER_H__
#define __TCP_SOCKET_HANDLER_H__

#include <functional>

namespace yuan::net
{
    class Connection;

    // 连接事件处理器接口
    class ConnectionHandler
    {
    public:
        virtual ~ConnectionHandler() = default;

        // 新连接建立（三次握手完成）
        virtual void on_connected(Connection *conn) = 0;
        
        // 连接发生错误
        virtual void on_error(Connection *conn) = 0;
        
        // 有数据可读
        virtual void on_read(Connection *conn) = 0;
        
        // 数据写入完成（写缓冲区有空间）
        virtual void on_write(Connection *conn) = 0;
        
        // 连接关闭
        virtual void on_close(Connection *conn) = 0;
    };

    // 扩展接口：支持中间件和过滤的ConnectionHandler
    // 可以用于在on_read/on_write等回调中插入额外逻辑
    template<typename BaseHandler>
    class FilteredConnectionHandler : public BaseHandler
    {
    public:
        using filter_func = std::function<bool(Connection*)>;
        
        explicit FilteredConnectionHandler(filter_func read_filter = nullptr,
                                           filter_func write_filter = nullptr)
            : read_filter_(std::move(read_filter))
            , write_filter_(std::move(write_filter))
        {}
        
        void set_read_filter(filter_func fn) { read_filter_ = std::move(fn); }
        void set_write_filter(filter_func fn) { write_filter_ = std::move(fn); }

    protected:
        filter_func read_filter_;
        filter_func write_filter_;
    };
}
#endif
