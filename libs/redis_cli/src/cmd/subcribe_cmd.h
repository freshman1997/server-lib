#ifndef __YUAN_REDIS_SUBCRIBE_CMD_H__
#define __YUAN_REDIS_SUBCRIBE_CMD_H__
#include "default_cmd.h"
#include "redis_value.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <set>

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

        std::unordered_map<std::string, std::shared_ptr<RedisValue>> get_messages()
        {
            return messages_;
        }

        void set_callback(std::function<void(const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &)> callback)
        {
            callback_ = callback;
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
            for (auto &channel : channels)
            {
                channels_.erase(channel);
            }

            if (channels_.empty()) {
                is_subcribe_ = false;
            }
        }

        void unsubcribe(const std::string &channel)
        {
            channels_.erase(channel);
            if (channels_.empty()) {
                is_subcribe_ = false;
            }
        }

        void unsubcribe_all()
        {
            channels_.clear();
            is_subcribe_ = false;
        }

    private:
        bool is_subcribe_ = false;
        std::set<std::string> channels_;
        std::unordered_map<std::string, std::shared_ptr<RedisValue>> messages_;
        std::function<void(const std::unordered_map<std::string, std::shared_ptr<RedisValue>> &)> callback_;
    };
}

#endif // __YUAN_REDIS_SUBCRIBE_CMD_H__