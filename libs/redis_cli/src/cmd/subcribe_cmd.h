#ifndef __YUAN_REDIS_SUBCRIBE_CMD_H__
#define __YUAN_REDIS_SUBCRIBE_CMD_H__
#include "default_cmd.h"
#include "redis_client.h"
#include "value/array_value.h"
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <vector>

namespace yuan::redis 
{
    class SubcribeCmd : public DefaultCmd
    {
    public:
        SubcribeCmd() = default;
        ~SubcribeCmd() = default;
        
        void set_channels(const std::vector<std::string> &channels)
        {
            for (auto &channel : channels)
            {
                channels_.insert(channel);
            }
        }

        void set_msg_callback(std::function<void(const std::vector<SubMessage> &messages)> msg_callback)
        {
            msg_callback_ = msg_callback;
        }

        void set_pmsg_callback(std::function<void(const std::vector<PSubMessage> &messages)> pmsg_callback)
        {
            pmsg_callback_ = pmsg_callback;
        }

        void exec_callback();

        virtual int unpack(buffer::BufferReader& reader);

        void set_is_subcribe(bool is_subcribe)
        {
            is_subcribe_ = is_subcribe;
        }

        bool is_subcribe() const
        {
            return is_subcribe_;
        }

        void unsubcribe(const std::vector<std::string> &channels) 
        {
            if (channels.empty()) {
                unsubcribe_all();
                return;
            }

            for (auto &channel : channels)
            {
                unsubcribe(channel);
            }
        }

        void unsubcribe(const std::string &channel)
        {
            if (channel.empty()) channels_.clear();
            else channels_.erase(channel);

            if (channels_.empty()) {
                is_subcribe_ = false;
            }
        }

        void unsubcribe_all()
        {
            channels_.clear();
            is_subcribe_ = false;
        }

        void set_message_cmd(const std::string &message_cmd)
        {
            message_cmd_ = message_cmd;
        }

    private:
        int unpack_sub_message(std::shared_ptr<ArrayValue> msgs);
        int unpack_psub_message(std::shared_ptr<ArrayValue> msgs);

    private:
        bool is_subcribe_ = false;
        std::string message_cmd_;
        std::set<std::string> channels_;
        std::vector<SubMessage> messages_;
        std::vector<PSubMessage> pmessages_;
        std::function<void(const std::vector<SubMessage> &messages)> msg_callback_;
        std::function<void(const std::vector<PSubMessage> &pmessages)> pmsg_callback_;
    };
}

#endif // __YUAN_REDIS_SUBCRIBE_CMD_H__