#include "subcribe_cmd.h"
#include "internal/def.h"
#include "value/array_value.h"
#include "value/string_value.h"

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
                
                auto arr = result_->as<ArrayValue>();
                if (!arr || arr->get_values().size() != channels_.size()) {
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

        const auto &arr_data = arr->get_values();
        
        if (arr_data[0]->get_type() != resp_string) {
            return -1;
        }

        auto msg = arr_data[0]->as<StringValue>();
        if (!msg || msg->get_value() != "message") {
            return -1;
        }

        messages_.clear();

        for (int i = 1; i + 1 < arr_data.size(); i += 2) {
            if (arr_data[i]->get_type() != resp_string) {
                return -1;
            }
    
            auto channel = arr_data[i]->as<StringValue>();
            if (!channel) {
                return -1;
            }

            messages_[channel->get_value()] = arr_data[i + 1];
        }

        result_ = nullptr;
            
        return ret;
    }

    void SubcribeCmd::exec_callback()
    {
        if (callback_) {
            callback_(messages_);
        }
    }
}