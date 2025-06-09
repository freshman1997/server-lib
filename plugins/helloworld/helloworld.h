#ifndef __HELLOWORLD_H__
#define __HELLOWORLD_H__
#include "plugin/plugin.h"
#include "message/message_dispacher.h"

class HelloWorldPlugin : public yuan::plugin::Plugin
{
public:
    HelloWorldPlugin();
    ~HelloWorldPlugin();
    
public:
    virtual void on_loaded();

    virtual bool on_init(yuan::message::MessageDispatcher *dispatcher);

    virtual void on_release();

public:
    virtual void on_message(const yuan::message::Message *msg);

    std::set<uint32_t> get_interest_events() const;

private:
    yuan::message::MessageDispatcher *dispatcher_;
};

#endif // __HELLOWORLD_H__
