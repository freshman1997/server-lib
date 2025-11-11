#include "subcribe_cmd.h"
#include "internal/def.h"
#include "value/array_value.h"
#include "value/string_value.h"
#include <iostream>

namespace yuan::redis 
{
    int SubcribeCmd::unpack(buffer::BufferReader& reader)
    {
        if (!is_subcribe_) {
            int ret = DefaultCmd::unpack(reader);
            if (ret >= 0 && result_) {
                if (result_->get_type() != resp_array) {
                    return -1;
                }
                
                is_subcribe_ = true;
                
                return ret;
            }
        }

        result_ = nullptr;
        int ret = DefaultCmd::unpack(reader);
        if (ret < 0 || !result_ || result_->get_type() != resp_array){
            return ret;
        }

        auto arr = result_->as<ArrayValue>();
        if (!arr || arr->get_values().size() < 2) {
            return -1;
        }

        int type = 0;
        if (message_cmd_ == "message") {
            type = 1;
        } else if (message_cmd_ == "pmessage") {
            type = 2;
        }

        if (type == 0) {
            return -1;
        }

        messages_.clear();
        pmessages_.clear();

        const auto &msgs = arr->get_values();
        if (msgs[0]->get_type() == resp_string) {
            if (type == 1 && unpack_sub_message(arr) < 0) {
                return -1;
            } else if (type == 2 && unpack_psub_message(arr) < 0) {
                return -1;
            }
        } else if (msgs[0]->get_type() == resp_array) {
            for (int i = 0; i < msgs.size(); i++) {
                auto msg = msgs[i]->as<ArrayValue>();
                if (!msg) {
                    return -1;
                }
                
                if (type == 1 && unpack_sub_message(msg) < 0) {
                    return -1;
                } else if (type == 2 && unpack_psub_message(msg) < 0) {
                    return -1;
                }
            }
        } else {
            return -1;
        }

        result_ = nullptr;
            
        return ret;
    }

    int SubcribeCmd::unpack_sub_message(std::shared_ptr<ArrayValue> msgs)
    {
        const auto &arr_data = msgs->get_values();
        if (arr_data.size() < 2) {
            return -1;
        }

        for (int i = 1; i + 2 < arr_data.size(); i += 3) {
            if (arr_data[i]->get_type() != resp_string) {
                return -1;
            }
    
            auto cmd = arr_data[i]->as<StringValue>();
            if (!cmd || cmd->get_value() != message_cmd_) {
                return -1;
            }

            if (arr_data[i + 1]->get_type() != resp_string) {
                return -1;
            }

            auto channel = arr_data[i + 1]->as<StringValue>();
            if (!channel) {
                return -1;
            }

            if (arr_data[i + 2]->get_type() != resp_string) {
                return -1;
            }

            auto message = arr_data[i + 2]->as<StringValue>();
            if (!message) {
                return -1;
            }

            messages_.emplace_back(channel, message);
        }

        return messages_.empty() ? -1 : 0;
    }

    int SubcribeCmd::unpack_psub_message(std::shared_ptr<ArrayValue> msgs)
    {
        const auto &arr_data = msgs->get_values();
        if (arr_data.size() < 4) {
            return -1;
        }

        for (int i = 0; i + 3 < arr_data.size(); i += 4) {
            if (arr_data[i]->get_type() != resp_string) {
                return -1;
            }
    
            auto cmd = arr_data[i]->as<StringValue>();
            if (!cmd || cmd->get_value() != message_cmd_) {
                return -1;
            }

            if (arr_data[i + 1]->get_type() != resp_string) {
                return -1;
            }

            auto pattern = arr_data[i + 1]->as<StringValue>();
            if (!pattern) {
                return -1;
            }

            if (arr_data[i + 2]->get_type() != resp_string) {
                return -1;
            }

            auto channel = arr_data[i + 2]->as<StringValue>();
            if (!channel) {
                return -1;
            }

            if (arr_data[i + 3]->get_type() != resp_string) {
                return -1;
            }

            auto message = arr_data[i + 3]->as<StringValue>();
            if (!message) {
                return -1;
            }

            pmessages_.emplace_back(pattern, channel, message);
        }

        return pmessages_.empty() ? -1 : 0;
    }


    void SubcribeCmd::exec_callback()
    {
        if (!messages_.empty() && msg_callback_) {
            msg_callback_(messages_);
            return;
        }

        if (!pmessages_.empty() && pmsg_callback_) {
            pmsg_callback_(pmessages_);
        }
    }
}