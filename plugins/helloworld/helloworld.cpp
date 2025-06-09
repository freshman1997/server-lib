#include "helloworld.h"
#include "api/api.h"
#include "message/message.h"
#include "message/message_dispacher.h"

#include <iostream>

class HelloWorldMessage : public yuan::message::MessageDestructor
{
public:
    HelloWorldMessage(const std::string &msg) : msg_(std::move(msg))
    {}

public:
    std::string msg_;
};

HelloWorldPlugin::HelloWorldPlugin()
{

}

HelloWorldPlugin::~HelloWorldPlugin()
{

}

void HelloWorldPlugin::on_loaded()
{
    std::cout << "hello world on_loaded !!\n";
}

bool HelloWorldPlugin::on_init(yuan::message::MessageDispatcher *dispatcher)
{
    dispatcher_ = dispatcher;
    if (!dispatcher_) {
        std::cerr << "dispatcher is null!!! \n";
        return false;
    }

    std::cout << "hello world init success !!\n";

    dispatcher_->register_consumer(yuan::message::user_message_, this);

    yuan::message::Message *msg = new yuan::message::Message;
    msg->type_ = yuan::message::MessageType::user_message_;

    HelloWorldMessage *hmsg = new HelloWorldMessage("你好世界！！！");
    msg->data_ = hmsg;
    msg->event_id_ = 1001;
    
    dispatcher_->send_message(msg);

    return true;
}

void HelloWorldPlugin::on_release()
{
    std::cout << "hello world on_release !!\n";
    delete this;
}

void HelloWorldPlugin::on_message(const yuan::message::Message *msg)
{
    if (msg->type_ & yuan::message::MessageType::user_message_ && msg->event_id_ == 1001)
    {
        const HelloWorldMessage *hmsg = static_cast<const HelloWorldMessage *>(msg->data_);
        std::cout << "receive msg: " << hmsg->msg_ << " \n";
    }
}

std::set<uint32_t> HelloWorldPlugin::get_interest_events() const
{
    return {1001};
}

YUAN_API_C_EXPORT void * get_HelloWorld_plugin_instance()
{
    return new HelloWorldPlugin;
}