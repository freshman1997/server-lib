#include "../redis_impl.h"
#include "cmd/default_cmd.h"
#include "cmd/multi_cmd.h"
#include "redis_client.h"
#include "value/error_value.h"

namespace yuan::redis 
{
    bool RedisClient::multi()
    {
        if (impl_->multi_cmd_) {
            return false;
        }

        auto cmd = std::make_shared<MultiCmd>();
        cmd->set_args("multi", {});
       
        impl_->multi_cmd_ = cmd;

        return true;
    }

    std::shared_ptr<RedisValue> RedisClient::exec()
    {
        if (!impl_->multi_cmd_) {
            return ErrorValue::from_string("ERR: EXEC without MULTI");
        }

        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("exec", {});
        impl_->multi_cmd_->add_command(cmd);
        auto tmp = impl_->multi_cmd_;
        impl_->multi_cmd_ = nullptr;
        return impl_->execute_command(tmp);
    }
}
