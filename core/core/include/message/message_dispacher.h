#ifndef __MESSAGE_DISPACHER_H__
#define __MESSAGE_DISPACHER_H__
#include "message/message.h"

namespace yuan::message 
{
    class MessageDispatcher : public message::MessageConsumer
    {
        friend class PluginManager;

    public:
        MessageDispatcher();
        ~MessageDispatcher();

        MessageDispatcher(const MessageDispatcher &) = delete;
        MessageDispatcher & operator=(const MessageDispatcher &) = delete;

        static MessageDispatcher * get_instance();

     public:
        virtual void on_message(const Message *msg);

        virtual bool need_free();

        virtual std::set<uint32_t> get_interest_events() const;

    public:
        bool init();

        int send_message(const void *msg);

        bool register_consumer(int msgType, void *consumer);

        void dispatch();

    private:
        void dispatch_message(const Message *msg);

    private:
        class InnerData;
        InnerData *data_;
    };
}

#endif // __MESSAGE_DISPACHER_H__
