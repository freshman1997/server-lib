#include "cmd/default_cmd.h"
#include "redis_client.h"
#include "value/string_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::auth(std::string password)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("auth", {std::make_shared<StringValue>(password)});
        return impl_->execute_command(cmd);
    }

    // info
    std::shared_ptr<RedisValue> RedisClient::info(std::string section /*= ""*/)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("info", {});
        
        if (!section.empty()) {
            cmd->add_arg(std::make_shared<StringValue>(section));
        }

        return impl_->execute_command(cmd);
    }

    // connect
    std::shared_ptr<RedisValue> RedisClient::ping()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("ping", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::echo(std::string message)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("echo", {std::make_shared<StringValue>(message)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::select(int index)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("select", {std::make_shared<StringValue>(std::to_string(index))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::quit()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("quit", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::swapdb(int index1, int index2)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("swapdb", {std::make_shared<StringValue>(std::to_string(index1)),
                                 std::make_shared<StringValue>(std::to_string(index2))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::time()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("time", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::wait(int numreplicas, int timeout)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("wait", {std::make_shared<StringValue>(std::to_string(numreplicas)),
                               std::make_shared<StringValue>(std::to_string(timeout))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bgrewriteaof()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("bgrewriteaof", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::bgsave()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("bgsave", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_getname()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("getname")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_id()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("id")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_list()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("list")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_pause(int timeout)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("pause"),
                                 std::make_shared<StringValue>(std::to_string(timeout))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_reply(std::string mode)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("reply"),
                                 std::make_shared<StringValue>(mode)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_setname(std::string name)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("setname"),
                                 std::make_shared<StringValue>(name)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::client_unblock(int id)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("client", {std::make_shared<StringValue>("unblock"),
                                 std::make_shared<StringValue>(std::to_string(id))});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::command()
    {
        auto cmd = std::make_shared<DefaultCmd>("command");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::command_count()
    {
        auto cmd = std::make_shared<DefaultCmd>();  
        cmd->set_args("command", {std::make_shared<StringValue>("count")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::command_getkeys()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("command", {std::make_shared<StringValue>("getkeys")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::command_info(const std::vector<std::string> &commands)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("command", {std::make_shared<StringValue>("info")});
        for (auto &command : commands) {
            cmd->add_arg(std::make_shared<StringValue>(command));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::config_get(std::string parameter)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("config", {std::make_shared<StringValue>("get"),
                                 std::make_shared<StringValue>(parameter)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::config_rewrite()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("config", {std::make_shared<StringValue>("rewrite")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::config_set(std::string parameter, std::string value)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("config", {std::make_shared<StringValue>("set"),
                                 std::make_shared<StringValue>(parameter),
                                 std::make_shared<StringValue>(value)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::config_resetstat()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("config", {std::make_shared<StringValue>("resetstat")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::dbsize()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("dbsize", {});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::debug_object(std::string key)
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("debug", {std::make_shared<StringValue>("object"),
                                std::make_shared<StringValue>(key)});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::debug_segfault()
    {
        auto cmd = std::make_shared<DefaultCmd>();
        cmd->set_args("debug", {std::make_shared<StringValue>("segfault")});
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::flushall(bool async /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>("flushall");
        if (async) {
            cmd->add_arg(std::make_shared<StringValue>("async"));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::flushdb(bool async /*= false*/)
    {
        auto cmd = std::make_shared<DefaultCmd>("flushdb");
        if (async) {
            cmd->add_arg(std::make_shared<StringValue>("async"));
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lastsave()
    {
        auto cmd = std::make_shared<DefaultCmd>("lastsave");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::monitor()
    {
        auto cmd = std::make_shared<DefaultCmd>("monitor");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::role()
    {
        auto cmd = std::make_shared<DefaultCmd>("role");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::save()
    {
        auto cmd = std::make_shared<DefaultCmd>("save");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown()
    {
        auto cmd = std::make_shared<DefaultCmd>("shutdown");
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save)
    {
        auto cmd = std::make_shared<DefaultCmd>("shutdown");
        cmd->add_arg(std::make_shared<StringValue>(save));
        return impl_->execute_command(cmd);
    }
}