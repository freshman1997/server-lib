#ifndef __YUAN_REDIS_SUBCRIBE_CMD_H__
#define __YUAN_REDIS_SUBCRIBE_CMD_H__
#include "default_cmd.h"
#include "redis_client.h"
#include "value/array_value.h"
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <functional>
#include <set>
#include <utility>
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
            add_channels(channels);
        }

        void add_channels(const std::vector<std::string> &channels)
        {
            for (const auto &channel : channels) {
                if (!channel.empty()) {
                    channels_.insert(channel);
                }
            }
        }

        void add_patterns(const std::vector<std::string> &patterns)
        {
            for (const auto &pattern : patterns) {
                if (!pattern.empty()) {
                    patterns_.insert(pattern);
                }
            }
        }

        void set_max_pending_messages(std::size_t max_pending_messages)
        {
            max_pending_messages_ = max_pending_messages == 0 ? kDefaultMaxPendingMessages : max_pending_messages;
        }

        void set_msg_callback(std::function<void(const std::vector<SubMessage> &messages)> msg_callback)
        {
            if (msg_callback) {
                msg_callback_ = std::move(msg_callback);
            }
        }

        void set_pmsg_callback(std::function<void(const std::vector<PSubMessage> &messages)> pmsg_callback)
        {
            if (pmsg_callback) {
                pmsg_callback_ = std::move(pmsg_callback);
            }
        }

        void exec_callback();

        bool has_pending_messages() const
        {
            return !pending_messages_.empty() || !pending_pmessages_.empty();
        }

        virtual int unpack(buffer::ByteBufferReader &reader);

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
                channels_.clear();
                if (patterns_.empty()) {
                    is_subcribe_ = false;
                    server_subscription_count_ = 0;
                }
                return;
            }

            for (auto &channel : channels) {
                unsubcribe(channel);
            }
        }

        void unsubcribe(const std::string &channel)
        {
            if (channel.empty())
                channels_.clear();
            else
                channels_.erase(channel);

            if (channels_.empty() && patterns_.empty()) {
                is_subcribe_ = false;
                server_subscription_count_ = 0;
            }
        }

        void unsubcribe_all()
        {
            channels_.clear();
            patterns_.clear();
            is_subcribe_ = false;
            server_subscription_count_ = 0;
        }

        void punsubcribe(const std::vector<std::string> &patterns)
        {
            if (patterns.empty()) {
                patterns_.clear();
            } else {
                for (const auto &pattern : patterns) {
                    patterns_.erase(pattern);
                }
            }

            if (channels_.empty() && patterns_.empty()) {
                is_subcribe_ = false;
                server_subscription_count_ = 0;
            }
        }

        std::size_t dropped_message_batches() const
        {
            return dropped_message_batches_;
        }

    private:
        int handle_frame(const std::shared_ptr<ArrayValue> &frame);
        int unpack_sub_message(const std::shared_ptr<ArrayValue> &frame);
        int unpack_psub_message(const std::shared_ptr<ArrayValue> &frame);
        int update_subscription_state(const std::string &kind, const std::shared_ptr<ArrayValue> &frame);
        void enqueue_messages();

        template <typename T>
        void trim_pending_queue(std::deque<std::vector<T>> &queue);

    private:
        static constexpr std::size_t kDefaultMaxPendingMessages = 65536;
        bool is_subcribe_ = false;
        int64_t server_subscription_count_ = 0;
        std::set<std::string> channels_;
        std::set<std::string> patterns_;
        std::vector<SubMessage> messages_;
        std::vector<PSubMessage> pmessages_;
        std::deque<std::vector<SubMessage> > pending_messages_;
        std::deque<std::vector<PSubMessage> > pending_pmessages_;
        std::size_t pending_message_count_ = 0;
        std::size_t max_pending_messages_ = kDefaultMaxPendingMessages;
        std::size_t dropped_message_batches_ = 0;
        std::function<void(const std::vector<SubMessage> &messages)> msg_callback_;
        std::function<void(const std::vector<PSubMessage> &pmessages)> pmsg_callback_;
    };
}

#endif // __YUAN_REDIS_SUBCRIBE_CMD_H__
