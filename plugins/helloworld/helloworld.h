#ifndef __HELLOWORLD_H__
#define __HELLOWORLD_H__
#include "plugin/plugin.h"

namespace yuan::plugin
{
    class PluginManager;
}

class HelloWorldPlugin : public yuan::plugin::Plugin
{
public:
    HelloWorldPlugin();
    ~HelloWorldPlugin();
    
public:
    virtual void on_loaded();

    virtual bool on_init();

    virtual void on_release();

private:
    yuan::plugin::PluginManager *plugin_manager_;
};

#endif // __HELLOWORLD_H__