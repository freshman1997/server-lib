#include "message/message.h"
#include "message/message_dispacher.h"
#include <queue>

namespace yuan::message 
{
    class MessageDispatcher::InnerData
    {
    public:
        InnerData() = default;

    public:
        std::unordered_map<int, std::vector<MessageConsumer *>> consumers_;
        std::queue<const Message *> messages_queue_;
    };

    MessageDispatcher::MessageDispatcher() : data_(new MessageDispatcher::InnerData)
    {
    }

    MessageDispatcher::~MessageDispatcher()
    {
        delete data_;
    }

    MessageDispatcher * MessageDispatcher::get_instance()
    {
        static MessageDispatcher instance;
        return &instance;
    }

    bool MessageDispatcher::init()
    {
        return true;
    }

    int MessageDispatcher::send_message(const void *msg)
    {
        data_->messages_queue_.push(static_cast<const Message *>(msg));
        return 0;
    }

    void MessageDispatcher::register_consumer(int msgTypes, void *consumer)
    {
        data_->consumers_[msgTypes].push_back(static_cast<MessageConsumer *>(consumer));
    }

    void MessageDispatcher::dispatch()
    {
        while (!data_->messages_queue_.empty()) {
            auto msg = data_->messages_queue_.front();
            data_->messages_queue_.pop();
            dispatch_message(msg);
        }
    }

    void MessageDispatcher::dispatch_message(const Message *msg)
    {
        uint32_t type = msg->type_;
        for (auto &item : data_->consumers_) {
            if (item.first & type) {
                for (auto &consumer : item.second) {
                    consumer->on_message(msg);
                }
            }
        }
        delete msg;
    }
}