#ifndef __ASYNC_APPENDER_H__
#define __ASYNC_APPENDER_H__

#include "log.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace yuan::log
{

/**
 * 异步追加器。
 *
 * 作用：
 * - 业务线程只负责投递 `LogItem`
 * - 后台线程负责从队列取出并交给 sink 落地
 * - 停止时会尽量把队列里剩余日志刷完
 */
class AsyncAppender
{
public:
    using Sink = std::function<void(const LogItem&)>;

    /// `max_queue_size == 0` 表示不限制队列长度。
    explicit AsyncAppender(size_t max_queue_size = 10000);
    ~AsyncAppender();

    AsyncAppender(const AsyncAppender&) = delete;
    AsyncAppender& operator=(const AsyncAppender&) = delete;

    void append(const LogItem& item);
    void set_sink(Sink sink);
    void start();
    void stop();
    void flush();
    size_t pending_count() const;

private:
    void thread_func();
    void consume_item(const LogItem& item);

private:
    std::queue<LogItem> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
    std::condition_variable cond_flushed_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    size_t max_queue_size_;
    bool processing_ = false;
    std::unique_ptr<std::thread> worker_thread_;
    Sink sink_;
    mutable std::mutex sink_mutex_;
};

} // namespace yuan::log

#endif // __ASYNC_APPENDER_H__
