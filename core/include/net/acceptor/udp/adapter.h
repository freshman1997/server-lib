#ifndef __NET_BASE_ACCEPTOR_UDP_ADAPTER_H__
#define __NET_BASE_ACCEPTOR_UDP_ADAPTER_H__

namespace yuan::timer 
{
    class TimerManager;
}

namespace yuan::net 
{
    class Connection;

    class UdpAdapter
    {
    public:
        /**
         * @brief 初始化时被调用
         * 
         * @param conn 当前连接对象
         * @param timerManager 定时器管理类对象
         * @return true 初始化成功
         * @return false 初始化失败
         */
        virtual bool init(Connection *conn, timer::TimerManager *timerManager) = 0;

        /**
         * @brief 收到原始数据包时被调用
         */
        virtual bool on_recv() = 0;

        /**
         * @brief 发送原始数据包时被调用
         * @return int 发送的数量
         */
        virtual int on_write() = 0;

        /**
         * @brief 需要释放时被调用
         */
        virtual void on_release() = 0;
    };
}

#endif