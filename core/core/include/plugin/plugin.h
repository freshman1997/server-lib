#ifndef __PLUGIN_H__
#define __PLUGIN_H__
#include "message/message.h"

namespace yuan::message
{
    class MessageDispatcher;
}

namespace yuan::plugin 
{
    class Plugin : public message::MessageConsumer
    {
    public:
        virtual void on_loaded() = 0;

        virtual bool on_init(message::MessageDispatcher *dispatcher) = 0;

        virtual void on_release() = 0;

        virtual void on_message(const message::Message *msg) {}

    public:
        virtual bool need_free() { return false; }
    };
}

#endif // __PLUGIN_H__
