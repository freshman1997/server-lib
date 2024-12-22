#ifndef __HELLOWORLD_H__
#define __HELLOWORLD_H__
#include "plugin/plugin.h"

class HelloWorldPlugin : public yuan::plugin::Plugin
{
public:
    virtual void on_loaded();

    virtual bool on_init();

    virtual void on_release();
};

#endif // __HELLOWORLD_H__