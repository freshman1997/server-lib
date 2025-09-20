#ifndef __APP_H__
#define __APP_H__
#include "singleton/singleton.h"

#include <memory>

namespace yuan::app
{
    class Service;
    
    class App : public singleton::Singleton<App>
    {
    public:
        App();
        virtual ~App();
        
    public:
        void launch();

        void exit();
        
        // 注册服务
        void add_service(std::shared_ptr<Service> service);
        
    private:
        class AppData;
        std::unique_ptr<AppData> data_;
    };
}

#endif
