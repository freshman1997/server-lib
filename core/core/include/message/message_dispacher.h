#ifndef __MESSAGE_DISPACHER_H__
#define __MESSAGE_DISPACHER_H__
#include "message/message.h"

namespace yuan::message 
{
    class MessageDispatcher
    {
    public:
        MessageDispatcher();
        ~MessageDispatcher();

        MessageDispatcher(const MessageDispatcher &) = delete;
        MessageDispatcher & operator=(const MessageDispatcher &) = delete;

        static MessageDispatcher * get_instance();
        
        bool init();

        int send_message(const void *msg);

        void register_consumer(int msgTypes, void *consumer);

        void dispatch();

    private:
        void dispatch_message(const Message *msg);

    private:
        class InnerData;
        InnerData *data_;
    };
}

#endif // __MESSAGE_DISPACHER_H__
