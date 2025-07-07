#include "message/message.h"
#include "message/message_dispacher.h"
#include "plugin/plugin_manager.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <unordered_map>

const static uint32_t MAX_CAPACITY = 10000;

namespace yuan::message 
{
    struct MessageConsumerHolder
    {

    };

    class MessageDispatcher::InnerData
    {
    public:
        InnerData() = default;

        ~InnerData()
        {
            while (!messages_queue_.empty()) {
                delete messages_queue_.front();
                messages_queue_.pop();
            }

            std::set<MessageConsumer *> consumersToFree;
            for (auto &typePair : consumers_) {
                MessageConsumer *consumer = nullptr;
                for (auto &eventPair : typePair.second) {
                    if (eventPair.second && eventPair.second->need_free() && consumersToFree.find(eventPair.second) == consumersToFree.end()) {
                        consumersToFree.insert(eventPair.second);
                    }
                }
            }

            for (auto *consumer : consumersToFree) {
                delete consumer;
            }

            consumersToFree.clear();
            consumers_.clear();
        }
        
    public:
        std::unordered_map<int, std::unordered_map<unsigned int, MessageConsumer *>> consumers_;
        std::queue<const Message *> messages_queue_;
        std::mutex mutex_;
        std::condition_variable cvar_;
    };

    MessageDispatcher::MessageDispatcher() : data_(new MessageDispatcher::InnerData)
    {
    }

    MessageDispatcher::~MessageDispatcher()
    {
        
    }

    MessageDispatcher * MessageDispatcher::get_instance()
    {
        static MessageDispatcher instance;
        return &instance;
    }

    bool MessageDispatcher::init()
    {
        register_consumer(system_message_, this);
        return true;
    }

    int MessageDispatcher::send_message(const void *msg)
    {
        std::lock_guard lock(data_->mutex_);
        if (data_->messages_queue_.size() >= MAX_CAPACITY) {
            std::unique_lock ulock(data_->mutex_);
            data_->cvar_.wait(ulock);
        }

        data_->messages_queue_.push(static_cast<const Message *>(msg));

        return 0;
    }

    bool MessageDispatcher::register_consumer(int msgType, void *consumer)
    {
        std::lock_guard lock(data_->mutex_);

        if (!consumer) {
            return false;
        }

        auto messageConsumer = static_cast<MessageConsumer *>(consumer);
        const auto &evs = messageConsumer->get_interest_events();
        if (evs.empty() || evs.size() > 100) {
            return false;
        }

        for (uint32_t event : evs) {
            data_->consumers_[msgType][event] = messageConsumer;
        }

        return true;
    }

    bool MessageDispatcher::register_consumer(int msgType, uint32_t eventId, void *consumer)
    {
        std::lock_guard lock(data_->mutex_);

        if (!consumer) {
            return false;
        }

        auto messageConsumer = static_cast<MessageConsumer *>(consumer);
        if (eventId == 0) {
            return false;
        }

        data_->consumers_[msgType][eventId] = messageConsumer;

        return true;
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
        const auto typeIter = data_->consumers_.find(msg->type_);
        if (typeIter != data_->consumers_.end()) {
            const auto it = typeIter->second.find(msg->event_id_);
            if (it != typeIter->second.end()) {
                it->second->on_message(msg);
            }
        }

        delete msg;
    }

    void MessageDispatcher::on_message(const Message *msg)
    {
        if (msg->type_ == message::system_message_ && msg->data_) {
            const message::SystemMessage *sysmsg = static_cast<const message::SystemMessage *>(msg->data_);
            const auto consumer = std::any_cast<MessageConsumer *>(sysmsg->data_);
            if (consumer) {
                switch (msg->event_id_) {
                case message::SystemMessage::SystemMessageType::free_msg_consumer_:
                {
                    for (auto &item : data_->consumers_)
                    {
                        const auto &evs = consumer->get_interest_events();
                        for (uint32_t event : evs) {
                            auto it = item.second.find(event);
                            if (it != item.second.end())
                            {
                                item.second.erase(it);
                            }
                        }

                        if (consumer->need_free())
                        {
                            delete consumer;
                            break;
                        }
                    }
                    break;
                }
                default: {
                    std::cerr << "cant handle consumer message: " << msg->event_id_ << " !!\n";
                    break;
                }
                }
            }
        }
    }

    bool MessageDispatcher::need_free()
    {
        return false;
    }

    std::set<uint32_t> MessageDispatcher::get_interest_events() const
    {
        return {message::SystemMessage::SystemMessageType::free_msg_consumer_};
    }
}