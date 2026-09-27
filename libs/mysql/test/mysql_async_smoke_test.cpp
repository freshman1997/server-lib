#include "yuan/mysql/mysql_client.h"

#include "mysql_smoke_options.h"

#include "coroutine/sync_wait.h"
#include "event/event_loop.h"
#include "net/poller/select_poller.h"
#include "timer/wheel_timer_manager.h"

#include <cstdlib>
#include <exception>
#include <iostream>

namespace
{
    yuan::coroutine::Task<int> run_async_smoke(yuan::coroutine::RuntimeView runtime,
                                              yuan::mysql::ConnectionOptions options)
    {
        yuan::mysql::AsyncConnection connection(runtime, std::move(options));
        co_await connection.connect_async();
        auto transaction = co_await connection.transaction_async();
        co_await transaction.execute_async("SET @yuan_mysql_async_smoke = ?", {yuan::mysql::string_value("mysql-worker")});
        co_await transaction.commit_async();

        const auto result = co_await connection.query_async("SELECT @yuan_mysql_async_smoke");
        if (result.row_count() != 1 || result.column_count() != 1 || !result.rows()[0][0] ||
            *result.rows()[0][0] != "mysql-worker") {
            std::cerr << "unexpected async smoke test result\n";
            co_return 1;
        }

        co_return 0;
    }
}

int main(int argc, char **argv)
{
    try {
        yuan::timer::WheelTimerManager timer_manager;
        yuan::net::SelectPoller poller;
        yuan::net::EventLoop loop(&poller, &timer_manager);
        yuan::coroutine::RuntimeView runtime(&loop, &timer_manager);

        const int result = yuan::coroutine::sync_wait(
            runtime,
            run_async_smoke(runtime, yuan::mysql::test::parse_connection_options(argc, argv)));
        if (result != 0) {
            return result;
        }

        std::cout << "YuanMysql async smoke test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
