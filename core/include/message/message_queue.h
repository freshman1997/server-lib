#ifndef __MESSAGE_QUEUE_H__
#define __MESSAGE_QUEUE_H__

namespace yuan::message
{
    class MPMCMessageQueue
    {
    public:
        void send_message();
    };
}

#endif // __MESSAGE_QUEUE_H__
