#ifndef __PLUGIN_H__
#define __PLUGIN_H__
#include <string>

namespace yuan::plugin 
{
    class Plugin
    {
    public:
        virtual void on_loaded() = 0;

        virtual bool on_init() = 0;

        virtual int on_message(const std::string &message) = 0;

        virtual void on_release() = 0;
    };
}

#endif // __PLUGIN_H__
