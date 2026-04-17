#include "subcribe_cmd.h"
#include "internal/def.h"
#include "value/array_value.h"
#include "value/int_value.h"
#include "value/string_value.h"
#include <iostream>

namespace yuan::redis
{
    int SubcribeCmd::unpack(buffer::ByteBufferReader & reader)
    {
        messages_.clear();
        pmessages_.clear();

        result_ = nullptr;
        const int ret = DefaultCmd::unpack(reader);
        if (ret < 0 || !result_ || result_->get_type() != resp_array) {
            return ret;
        }

        auto arr = result_->as<ArrayValue>();
        if (!arr || arr->get_values().empty()) {
            return -1;
        }

        const auto &frames = arr->get_values();
        if (frames[0] && frames[0]->get_type() == resp_array) {
            result_ = nullptr;
            for (const auto &frame_value : frames) {
                const auto frame = frame_value ? frame_value->as<ArrayValue>() : nullptr;
                if (!frame || handle_frame(frame) < 0) {
                    return -1;
                }
            }
        } else if (handle_frame(arr) < 0) {
            return -1;
        }

        if (!messages_.empty()) {
            pending_messages_.push_back(messages_);
        }

        if (!pmessages_.empty()) {
            pending_pmessages_.push_back(pmessages_);
        }

        return ret;
    }

    int SubcribeCmd::handle_frame(const std::shared_ptr<ArrayValue> & frame)
    {
        if (!frame) {
            return -1;
        }

        const auto &values = frame->get_values();
        if (values.empty() || !values[0] || values[0]->get_type() != resp_string) {
            return -1;
        }

        const auto kind = values[0]->as<StringValue>();
        if (!kind) {
            return -1;
        }

        if (kind->get_value() == "message") {
            if (result_.get() == frame.get()) {
                result_ = nullptr;
            }
            return unpack_sub_message(frame);
        }

        if (kind->get_value() == "pmessage") {
            if (result_.get() == frame.get()) {
                result_ = nullptr;
            }
            return unpack_psub_message(frame);
        }

        if (kind->get_value() == "subscribe" || kind->get_value() == "psubscribe" || kind->get_value() == "unsubscribe" || kind->get_value() == "punsubscribe") {
            return update_subscription_state(kind->get_value(), frame);
        }

        return -1;
    }

    int SubcribeCmd::unpack_sub_message(const std::shared_ptr<ArrayValue> & frame)
    {
        const auto &values = frame->get_values();
        if (values.size() != 3) {
            return -1;
        }

        const auto channel = values[1]->as<StringValue>();
        const auto message = values[2]->as<StringValue>();
        if (!channel || !message) {
            return -1;
        }

        messages_.emplace_back(channel, message);
        return 0;
    }

    int SubcribeCmd::unpack_psub_message(const std::shared_ptr<ArrayValue> & frame)
    {
        const auto &values = frame->get_values();
        if (values.size() != 4) {
            return -1;
        }

        const auto pattern = values[1]->as<StringValue>();
        const auto channel = values[2]->as<StringValue>();
        const auto message = values[3]->as<StringValue>();
        if (!pattern || !channel || !message) {
            return -1;
        }

        pmessages_.emplace_back(pattern, channel, message);
        return 0;
    }

    int SubcribeCmd::update_subscription_state(const std::string & kind, const std::shared_ptr<ArrayValue> & frame)
    {
        const auto &values = frame->get_values();
        if (values.size() < 3) {
            return -1;
        }

        const auto count = values[2] ? values[2]->as<IntValue>() : nullptr;
        if (!count) {
            return -1;
        }

        const auto target = values[1] ? values[1]->as<StringValue>() : nullptr;
        if (kind == "subscribe" || kind == "psubscribe") {
            if (target && !target->get_value().empty()) {
                channels_.insert(target->get_value());
            }
            is_subcribe_ = count->get_value() > 0;
        } else {
            if (target && !target->get_value().empty()) {
                channels_.erase(target->get_value());
            } else {
                channels_.clear();
            }
            is_subcribe_ = count->get_value() > 0 && !channels_.empty();
        }

        result_ = frame;
        return 0;
    }

    void SubcribeCmd::exec_callback()
    {
        if (!pending_messages_.empty() && msg_callback_) {
            auto messages = std::move(pending_messages_.front());
            pending_messages_.pop_front();
            msg_callback_(messages);
        }

        if (!pending_pmessages_.empty() && pmsg_callback_) {
            auto messages = std::move(pending_pmessages_.front());
            pending_pmessages_.pop_front();
            pmsg_callback_(messages);
        }
    }
}
