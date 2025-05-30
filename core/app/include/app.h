#ifndef __APP_H__
#define __APP_H__
#include "singleton/singleton.h"
#include <memory>

namespace yuan::app
{
    class App : public singleton::Singleton<App>
    {
    public:
        App();
        virtual ~App();
        
    public:
        void launch();

        void exit();
        
    private:
        class AppData;
        std::unique_ptr<AppData> data_;
    };
}

#endif
