#include "async_appender.h"

namespace yuan::log
{

AsyncAppender::AsyncAppender(size_t max_queue_size)
    : max_queue_size_(max_queue_size)
{}

AsyncAppender::~AsyncAppender()
{
    stop();
}

void AsyncAppender::set_sink(Sink sink)
{
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(sink);
}

void AsyncAppender::start()
{
    if (running_.exchange(true)) return;
    stopping_.store(false);
    worker_thread_ = std::make_unique<std::thread>(&AsyncAppender::thread_func, this);
}

void AsyncAppender::stop()
{
    if (!running_.exchange(false)) return;

    stopping_.store(true);
    cond_not_empty_.notify_all();

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    worker_thread_.reset();

    flush();
}

void AsyncAppender::append(const LogItem& item)
{
    if (!running_.load(std::memory_order_acquire)) {
        consume_item(item);
        return;
    }

    {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_not_full_.wait(lock, [&] {
            return !running_.load() ||
                   max_queue_size_ == 0 ||
                   queue_.size() < max_queue_size_;
        });

        if (!running_.load()) {
            lock.unlock();
            consume_item(item);
            return;
        }

        queue_.push(item);
    }
    cond_not_empty_.notify_one();
}

void AsyncAppender::thread_func()
{
    while (true) {
        LogItem item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_not_empty_.wait(lock, [&] {
                return !queue_.empty() || stopping_.load() || !running_.load();
            });

            if ((stopping_.load() || !running_.load()) && queue_.empty()) {
                break;
            }

            item = std::move(queue_.front());
            queue_.pop();
            processing_ = true;
        }
        cond_not_full_.notify_one();

        consume_item(item);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            processing_ = false;
            if (queue_.empty()) cond_flushed_.notify_all();
        }
    }
}

void AsyncAppender::flush()
{
    if (running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_flushed_.wait(lock, [&] { return queue_.empty() && !processing_; });
        return;
    }

    while (true) {
        LogItem item;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) break;
            item = std::move(queue_.front());
            queue_.pop();
        }
        cond_not_full_.notify_one();
        consume_item(item);
    }
}

void AsyncAppender::consume_item(const LogItem& item)
{
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(sink_mutex_);
        sink = sink_;
    }
    if (sink) sink(item);
}

size_t AsyncAppender::pending_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace yuan::log
