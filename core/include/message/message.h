#ifndef __MESSAGE_H__
#define __MESSAGE_H__
#include "net/socket/inet_address.h"
#include "net/connection/connection.h"

namespace yuan::message 
{
    enum MessageType : char
    {
        system_message_,
        net_message_,
        user_message_,
    };

    struct Message
    {
        struct
        {
            MessageType     type_;
            unsigned char   ext1_;
            unsigned char   ext2_;
            unsigned char   ext3_;
        }                   head_;
        
        unsigned int        event_;
        void *              data_;
    };

    struct SystemMessage
    {
        enum class SystemMessageType : char
        {
            start_,
            stop_,
            add_timer_,
            remove_timer_,
            load_plugin_,
            release_plugin_,
        };

        union SystemMessageData
        {
            unsigned int    timer_id_;
            std::string     plugin_name_;
        };
    };

    struct NetMessage
    {
        enum class NetMessageType : char
        {
            listen_,
            connect_,
            connected_,
            disconnected_,
            receive_,
            send_,
        };

        union NetMessageData
        {
            yuan::net::InetAddress    addr_;
            yuan::net::Connection *   conn_;
        };

        NetMessageData data_ = {};
    };
}

#endif // __MESSAGE_H__
