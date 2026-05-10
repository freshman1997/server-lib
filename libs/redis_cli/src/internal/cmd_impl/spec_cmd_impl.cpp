#include "internal/cmd_builder.h"
#include "redis_client.h"
#include "value/error_value.h"
#include "../redis_impl.h"

namespace yuan::redis 
{
    std::shared_ptr<RedisValue> RedisClient::auth(std::string password)
    {
        return impl_->execute_command(make_cmd("auth", password));
    }

    std::shared_ptr<RedisValue> RedisClient::auth(std::string username, std::string password)
    {
        return impl_->execute_command(make_cmd("auth", username, password));
    }

    // info
    std::shared_ptr<RedisValue> RedisClient::info(std::string section /*= ""*/)
    {
        auto cmd = make_cmd("info");
        
        if (!section.empty()) {
            append_arg(cmd, section);
        }

        return impl_->execute_command(cmd);
    }

    // connect
    std::shared_ptr<RedisValue> RedisClient::ping()
    {
        return impl_->execute_command(make_cmd("ping"));
    }

    std::shared_ptr<RedisValue> RedisClient::echo(std::string message)
    {
        return impl_->execute_command(make_cmd("echo", message));
    }

    std::shared_ptr<RedisValue> RedisClient::select(int index)
    {
        return impl_->execute_command(make_cmd("select", index));
    }

    std::shared_ptr<RedisValue> RedisClient::quit()
    {
        return impl_->execute_command(make_cmd("quit"));
    }

    std::shared_ptr<RedisValue> RedisClient::swapdb(int index1, int index2)
    {
        return impl_->execute_command(make_cmd("swapdb", index1, index2));
    }

    std::shared_ptr<RedisValue> RedisClient::time()
    {
        return impl_->execute_command(make_cmd("time"));
    }

    std::shared_ptr<RedisValue> RedisClient::wait(int numreplicas, int timeout)
    {
        return impl_->execute_command(make_cmd("wait", numreplicas, timeout));
    }

    std::shared_ptr<RedisValue> RedisClient::bgrewriteaof()
    {
        return impl_->execute_command(make_cmd("bgrewriteaof"));
    }

    std::shared_ptr<RedisValue> RedisClient::bgsave()
    {
        return impl_->execute_command(make_cmd("bgsave"));
    }

    std::shared_ptr<RedisValue> RedisClient::client_getname()
    {
        return impl_->execute_command(make_cmd("client", "getname"));
    }

    std::shared_ptr<RedisValue> RedisClient::client_id()
    {
        return impl_->execute_command(make_cmd("client", "id"));
    }

    std::shared_ptr<RedisValue> RedisClient::client_list()
    {
        return impl_->execute_command(make_cmd("client", "list"));
    }

    std::shared_ptr<RedisValue> RedisClient::client_pause(int timeout)
    {
        return impl_->execute_command(make_cmd("client", "pause", timeout));
    }

    std::shared_ptr<RedisValue> RedisClient::client_reply(std::string mode)
    {
        return impl_->execute_command(make_cmd("client", "reply", mode));
    }

    std::shared_ptr<RedisValue> RedisClient::client_setname(std::string name)
    {
        return impl_->execute_command(make_cmd("client", "setname", name));
    }

    std::shared_ptr<RedisValue> RedisClient::client_unblock(int id)
    {
        return impl_->execute_command(make_cmd("client", "unblock", id));
    }

    std::shared_ptr<RedisValue> RedisClient::command()
    {
        return impl_->execute_command(make_cmd("command"));
    }

    std::shared_ptr<RedisValue> RedisClient::command_count()
    {
        return impl_->execute_command(make_cmd("command", "count"));
    }

    std::shared_ptr<RedisValue> RedisClient::command_getkeys()
    {
        set_last_error(ErrorValue::from_string("ERR: COMMAND GETKEYS requires the raw command arguments"));
        return nullptr;
    }

    std::shared_ptr<RedisValue> RedisClient::command_getkeys(const std::vector<std::string> &command_args)
    {
        if (command_args.empty()) {
            set_last_error(ErrorValue::from_string("ERR: COMMAND GETKEYS requires at least one command argument"));
            return nullptr;
        }

        auto cmd = make_cmd("command", "getkeys");
        append_args(cmd, command_args);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::command_info(const std::vector<std::string> &commands)
    {
        auto cmd = make_cmd("command", "info");
        append_args(cmd, commands);
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::config_get(std::string parameter)
    {
        return impl_->execute_command(make_cmd("config", "get", parameter));
    }

    std::shared_ptr<RedisValue> RedisClient::config_rewrite()
    {
        return impl_->execute_command(make_cmd("config", "rewrite"));
    }

    std::shared_ptr<RedisValue> RedisClient::config_set(std::string parameter, std::string value)
    {
        return impl_->execute_command(make_cmd("config", "set", parameter, value));
    }

    std::shared_ptr<RedisValue> RedisClient::config_resetstat()
    {
        return impl_->execute_command(make_cmd("config", "resetstat"));
    }

    std::shared_ptr<RedisValue> RedisClient::dbsize()
    {
        return impl_->execute_command(make_cmd("dbsize"));
    }

    std::shared_ptr<RedisValue> RedisClient::debug_object(std::string key)
    {
        return impl_->execute_command(make_cmd("debug", "object", key));
    }

    std::shared_ptr<RedisValue> RedisClient::debug_segfault()
    {
        return impl_->execute_command(make_cmd("debug", "segfault"));
    }

    std::shared_ptr<RedisValue> RedisClient::flushall(bool async /*= false*/)
    {
        auto cmd = make_cmd("flushall");
        if (async) {
            append_arg(cmd, "async");
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::flushdb(bool async /*= false*/)
    {
        auto cmd = make_cmd("flushdb");
        if (async) {
            append_arg(cmd, "async");
        }
        return impl_->execute_command(cmd);
    }

    std::shared_ptr<RedisValue> RedisClient::lastsave()
    {
        return impl_->execute_command(make_cmd("lastsave"));
    }

    std::shared_ptr<RedisValue> RedisClient::monitor()
    {
        return impl_->execute_command(make_cmd("monitor"));
    }

    std::shared_ptr<RedisValue> RedisClient::role()
    {
        return impl_->execute_command(make_cmd("role"));
    }

    std::shared_ptr<RedisValue> RedisClient::save()
    {
        return impl_->execute_command(make_cmd("save"));
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown()
    {
        return impl_->execute_command(make_cmd("shutdown"));
    }

    std::shared_ptr<RedisValue> RedisClient::shutdown(std::string save)
    {
        return impl_->execute_command(make_cmd("shutdown", save));
    }
}
