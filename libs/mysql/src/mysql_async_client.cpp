#include "yuan/mysql/mysql_client.h"

#include "event/event_loop.h"

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>

namespace yuan::mysql
{
    struct AsyncConnectionState
    {
        explicit AsyncConnectionState(coroutine::RuntimeView runtime, ConnectionOptions options)
            : runtime(runtime), options(std::move(options)), worker([this] { run(); })
        {
        }

        ~AsyncConnectionState()
        {
            stop();
        }

        AsyncConnectionState(const AsyncConnectionState &) = delete;
        AsyncConnectionState &operator=(const AsyncConnectionState &) = delete;

        void submit(std::function<void(Connection &)> job)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) {
                    throw MysqlError(0, "HY000", "async mysql worker is stopped");
                }
                jobs.push(std::move(job));
            }
            cv.notify_one();
        }

        void stop()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (stopping) {
                    return;
                }
                stopping = true;
            }
            cv.notify_one();
            if (worker.joinable()) {
                worker.join();
            }
        }

        void run()
        {
            ThreadGuard mysql_thread;
            std::optional<Connection> connection;
            for (;;) {
                std::function<void(Connection &)> job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [this] { return stopping || !jobs.empty(); });
                    if (stopping && jobs.empty()) {
                        break;
                    }
                    job = std::move(jobs.front());
                    jobs.pop();
                }

                if (!connection) {
                    connection.emplace();
                }
                job(*connection);
            }
        }

        coroutine::RuntimeView runtime;
        ConnectionOptions options;
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::function<void(Connection &)>> jobs;
        std::thread worker;
        bool stopping = false;
    };

    namespace
    {
        template <typename T>
        class WorkerAwaiter
        {
        public:
            using Operation = std::function<T(Connection &)>;

            WorkerAwaiter(std::shared_ptr<AsyncConnectionState> state, Operation operation)
                : operation_(std::make_shared<OperationState>(std::move(state), std::move(operation)))
            {
            }

            bool await_ready() const noexcept
            {
                return false;
            }

            bool await_suspend(std::coroutine_handle<> continuation)
            {
                operation_->continuation = continuation;
                try {
                    if (!operation_->state) {
                        throw MysqlError(0, "HY000", "async mysql connection is closed");
                    }
                    operation_->state->submit([operation = operation_](Connection &connection) {
                        try {
                            if constexpr (std::is_void_v<T>) {
                                operation->operation(connection);
                            } else {
                                operation->result = operation->operation(connection);
                            }
                        } catch (...) {
                            operation->exception = std::current_exception();
                        }
                        operation->resume();
                    });
                } catch (...) {
                    operation_->exception = std::current_exception();
                    return false;
                }
                return true;
            }

            T await_resume()
            {
                if (operation_->exception) {
                    std::rethrow_exception(operation_->exception);
                }
                if constexpr (!std::is_void_v<T>) {
                    return std::move(*operation_->result);
                }
            }

        private:
            struct OperationState
            {
                OperationState(std::shared_ptr<AsyncConnectionState> state, Operation operation)
                    : state(std::move(state)), operation(std::move(operation))
                {
                }

                void resume() noexcept
                {
                    if (state->runtime.event_loop()) {
                        state->runtime.event_loop()->post_coroutine(continuation);
                    } else if (continuation) {
                        continuation.resume();
                    }
                }

                std::shared_ptr<AsyncConnectionState> state;
                Operation operation;
                std::coroutine_handle<> continuation;
                std::exception_ptr exception;
                std::optional<T> result;
            };

            std::shared_ptr<OperationState> operation_;
        };

        template <>
        class WorkerAwaiter<void>
        {
        public:
            using Operation = std::function<void(Connection &)>;

            WorkerAwaiter(std::shared_ptr<AsyncConnectionState> state, Operation operation)
                : operation_(std::make_shared<OperationState>(std::move(state), std::move(operation)))
            {
            }

            bool await_ready() const noexcept
            {
                return false;
            }

            bool await_suspend(std::coroutine_handle<> continuation)
            {
                operation_->continuation = continuation;
                try {
                    if (!operation_->state) {
                        throw MysqlError(0, "HY000", "async mysql connection is closed");
                    }
                    operation_->state->submit([operation = operation_](Connection &connection) {
                        try {
                            operation->operation(connection);
                        } catch (...) {
                            operation->exception = std::current_exception();
                        }
                        operation->resume();
                    });
                } catch (...) {
                    operation_->exception = std::current_exception();
                    return false;
                }
                return true;
            }

            void await_resume()
            {
                if (operation_->exception) {
                    std::rethrow_exception(operation_->exception);
                }
            }

        private:
            struct OperationState
            {
                OperationState(std::shared_ptr<AsyncConnectionState> state, Operation operation)
                    : state(std::move(state)), operation(std::move(operation))
                {
                }

                void resume() noexcept
                {
                    if (state->runtime.event_loop()) {
                        state->runtime.event_loop()->post_coroutine(continuation);
                    } else if (continuation) {
                        continuation.resume();
                    }
                }

                std::shared_ptr<AsyncConnectionState> state;
                Operation operation;
                std::coroutine_handle<> continuation;
                std::exception_ptr exception;
            };

            std::shared_ptr<OperationState> operation_;
        };
    }

    AsyncConnection::AsyncConnection(coroutine::RuntimeView runtime, ConnectionOptions options)
        : state_(std::make_shared<AsyncConnectionState>(runtime, std::move(options)))
    {
    }

    AsyncConnection::~AsyncConnection()
    {
        close_now();
    }

    AsyncConnection::AsyncConnection(AsyncConnection &&other) noexcept
        : state_(std::move(other.state_)), connected_(std::exchange(other.connected_, false))
    {
    }

    AsyncConnection &AsyncConnection::operator=(AsyncConnection &&other) noexcept
    {
        if (this != &other) {
            close_now();
            state_ = std::move(other.state_);
            connected_ = std::exchange(other.connected_, false);
        }
        return *this;
    }

    coroutine::Task<void> AsyncConnection::connect_async()
    {
        auto state = state_;
        co_await WorkerAwaiter<void>(state, [state](Connection &connection) {
            connection.connect(state->options);
        });
        connected_ = true;
    }

    coroutine::Task<void> AsyncConnection::close_async()
    {
        if (!state_) {
            co_return;
        }
        auto state = state_;
        co_await WorkerAwaiter<void>(state, [](Connection &connection) {
            connection.close();
        });
        connected_ = false;
    }

    coroutine::Task<void> AsyncConnection::execute_async(std::string sql)
    {
        co_await WorkerAwaiter<void>(state_, [sql = std::move(sql)](Connection &connection) {
            connection.execute(sql);
        });
    }

    coroutine::Task<void> AsyncConnection::execute_async(std::string sql, std::vector<Value> params)
    {
        co_await WorkerAwaiter<void>(state_, [sql = std::move(sql), params = std::move(params)](Connection &connection) {
            connection.execute(sql, params);
        });
    }

    coroutine::Task<Result> AsyncConnection::query_async(std::string sql)
    {
        co_return co_await WorkerAwaiter<Result>(state_, [sql = std::move(sql)](Connection &connection) {
            return connection.query(sql);
        });
    }

    coroutine::Task<Result> AsyncConnection::query_async(std::string sql, std::vector<Value> params)
    {
        co_return co_await WorkerAwaiter<Result>(state_, [sql = std::move(sql), params = std::move(params)](Connection &connection) {
            return connection.query(sql, params);
        });
    }

    coroutine::Task<void> AsyncConnection::begin_transaction_async()
    {
        co_await WorkerAwaiter<void>(state_, [](Connection &connection) {
            connection.begin_transaction();
        });
    }

    coroutine::Task<AsyncTransaction> AsyncConnection::transaction_async()
    {
        co_await begin_transaction_async();
        co_return AsyncTransaction(state_);
    }

    coroutine::Task<void> AsyncConnection::commit_async()
    {
        co_await WorkerAwaiter<void>(state_, [](Connection &connection) {
            connection.commit();
        });
    }

    coroutine::Task<void> AsyncConnection::rollback_async()
    {
        co_await WorkerAwaiter<void>(state_, [](Connection &connection) {
            connection.rollback();
        });
    }

    bool AsyncConnection::is_connected() const noexcept
    {
        return connected_;
    }

    void AsyncConnection::close_now() noexcept
    {
        if (state_) {
            state_->stop();
            state_.reset();
        }
        connected_ = false;
    }

    AsyncTransaction::AsyncTransaction(std::shared_ptr<AsyncConnectionState> state)
        : state_(std::move(state)), completed_(false)
    {
    }

    AsyncTransaction::~AsyncTransaction()
    {
        rollback_on_destroy();
    }

    AsyncTransaction::AsyncTransaction(AsyncTransaction &&other) noexcept
        : state_(std::move(other.state_)), completed_(std::exchange(other.completed_, true))
    {
    }

    AsyncTransaction &AsyncTransaction::operator=(AsyncTransaction &&other) noexcept
    {
        if (this != &other) {
            rollback_on_destroy();
            state_ = std::move(other.state_);
            completed_ = std::exchange(other.completed_, true);
        }
        return *this;
    }

    coroutine::Task<void> AsyncTransaction::execute_async(std::string sql)
    {
        co_await WorkerAwaiter<void>(state_, [sql = std::move(sql)](Connection &connection) {
            connection.execute(sql);
        });
    }

    coroutine::Task<void> AsyncTransaction::execute_async(std::string sql, std::vector<Value> params)
    {
        co_await WorkerAwaiter<void>(state_, [sql = std::move(sql), params = std::move(params)](Connection &connection) {
            connection.execute(sql, params);
        });
    }

    coroutine::Task<Result> AsyncTransaction::query_async(std::string sql)
    {
        co_return co_await WorkerAwaiter<Result>(state_, [sql = std::move(sql)](Connection &connection) {
            return connection.query(sql);
        });
    }

    coroutine::Task<Result> AsyncTransaction::query_async(std::string sql, std::vector<Value> params)
    {
        co_return co_await WorkerAwaiter<Result>(state_, [sql = std::move(sql), params = std::move(params)](Connection &connection) {
            return connection.query(sql, params);
        });
    }

    coroutine::Task<void> AsyncTransaction::commit_async()
    {
        co_await WorkerAwaiter<void>(state_, [](Connection &connection) {
            connection.commit();
        });
        completed_ = true;
    }

    coroutine::Task<void> AsyncTransaction::rollback_async()
    {
        co_await WorkerAwaiter<void>(state_, [](Connection &connection) {
            connection.rollback();
        });
        completed_ = true;
    }

    void AsyncTransaction::rollback_on_destroy() noexcept
    {
        if (completed_ || !state_) {
            return;
        }
        completed_ = true;
        try {
            state_->submit([](Connection &connection) {
                try {
                    connection.rollback();
                } catch (...) {
                    connection.close();
                }
            });
        } catch (...) {
        }
    }
}
