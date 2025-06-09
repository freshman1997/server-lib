#ifndef __MESSAGE_H__
#define __MESSAGE_H__
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"
#include <any>
#include <set>

namespace yuan::message 
{
    enum MessageType : char
    {
        null_message_   = 0,
        system_message_ = 1,
        net_message_    = 1 << 1,
        user_message_   = 1 << 2,
    };

    class MessageDestructor
    {
    public:
        virtual ~MessageDestructor() {}
    };

    struct Message
    {
        struct
        {
            MessageType     type_ = null_message_;
            unsigned char   ext1_ = 0;
            unsigned char   ext2_ = 0;
            unsigned char   ext3_ = 0;
        };
        
        unsigned int        event_id_ = 0;
        void *              data_  = nullptr;

        virtual ~Message()
        {
            if (data_) {
                delete static_cast<MessageDestructor *>(data_);
            }
        }
    };

    struct SystemMessage : public MessageDestructor
    {
        enum SystemMessageType : char
        {
            start_,
            stop_,
            add_timer_,
            remove_timer_,
            load_plugin_,
            load_plugin_result_,
            release_plugin_,
            free_msg_consumer_,
        };

        struct LoadPluginResult
        {
            std::string plugin_name_;
            bool result_;
        };

        std::any data_;
    };

    struct NetMessage : public MessageDestructor
    {
        enum NetMessageType : char
        {
            listen_,
            connect_,
            connected_,
            disconnected_,
            receive_,
            send_,
        };

        yuan::net::InetAddress    addr_;
        yuan::net::Connection *   conn_;
    };

    class MessageConsumer
    {
    public:
        virtual ~MessageConsumer() = default;

        virtual void on_message(const Message *msg) = 0;

        virtual bool need_free() = 0;

        virtual std::set<uint32_t> get_interest_events() const = 0;
    };
}

#endif // __MESSAGE_H__
