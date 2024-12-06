#ifndef __APP_H__
#define __APP_H__
#include "net/acceptor/acceptor.h"
#include "net/connector/connector.h"
#include "net/socket/inet_address.h"
#include <memory>

namespace app
{
    class App
    {
    public:
        App();
        virtual ~App();
        
    public:
        bool add_acceptor(const net::InetAddress &addr);
        
        bool add_connector(const net::InetAddress &addr);

        void launch();

        void exit();

        void set_ssl_module();
        
    public:
        void process_data();

    private:
        class AppData;
        std::unique_ptr<AppData> data_;
    };
}

#endif
