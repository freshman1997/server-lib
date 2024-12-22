#ifndef __MESSAGE_QUEUE_H__
#define __MESSAGE_QUEUE_H__
#include "singleton/singleton.h"

namespace yuan::message
{
    class MPMCMessageQueue : public singleton::Singleton<MPMCMessageQueue>
    {
    public:
        void send_message();
    };
}

#endif // __MESSAGE_QUEUE_H__
