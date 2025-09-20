#ifndef __SERVICE_H__
#define __SERVICE_H__

namespace yuan::app
{
    class Service
    {
    public:
        virtual ~Service() = default;

        // 初始化服务
        virtual void init() = 0;

        // 启动服务
        virtual void start() = 0;

        // 停止服务
        virtual void stop() = 0;
    };
}

#endif // __SERVICE_H__
