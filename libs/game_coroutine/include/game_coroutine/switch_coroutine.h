#ifndef __YUAN_GAME_COROUTINE_SWITCH_COROUTINE_H__
#define __YUAN_GAME_COROUTINE_SWITCH_COROUTINE_H__

#include <optional>
#include <string_view>
#include <string>
#include <vector>
#include <functional>
#include <utility>

namespace yuan::game_coroutine
{
    enum class SwitchCoroutineStatus
    {
        pending,
        completed,
        failed
    };

    enum class SwitchCoroutineError
    {
        none,
        failed,
        canceled,
        timed_out,
        invalid_state,
        conflict,
        insufficient_resource,
        external_error
    };

    enum class SwitchCoroutineTraceEvent
    {
        resume,
        suspend,
        complete,
        fail,
        cancel,
        timeout,
        compensation
    };

    struct SwitchCoroutineTrace
    {
        SwitchCoroutineTraceEvent event = SwitchCoroutineTraceEvent::resume;
        int state = 0;
        SwitchCoroutineError error = SwitchCoroutineError::none;
        std::string_view name;
        std::string_view message;
    };

    class SwitchCoroutineContext
    {
    public:
        using Compensation = std::function<void()>;
        using TraceHook = std::function<void(const SwitchCoroutineTrace &)>;

        int state() const noexcept
        {
            return state_;
        }

        void set_state(int state) noexcept
        {
            state_ = state;
        }

        const std::string &name() const noexcept
        {
            return name_;
        }

        void set_name(std::string name)
        {
            name_ = std::move(name);
        }

        bool finished() const noexcept
        {
            return status_ == SwitchCoroutineStatus::completed ||
                   status_ == SwitchCoroutineStatus::failed;
        }

        bool completed() const noexcept
        {
            return status_ == SwitchCoroutineStatus::completed;
        }

        bool failed() const noexcept
        {
            return status_ == SwitchCoroutineStatus::failed;
        }

        SwitchCoroutineStatus status() const noexcept
        {
            return status_;
        }

        void complete() noexcept
        {
            status_ = SwitchCoroutineStatus::completed;
            state_ = kFinishedState;
            trace(SwitchCoroutineTraceEvent::complete);
        }

        void fail(std::string error = {}, SwitchCoroutineError code = SwitchCoroutineError::failed)
        {
            status_ = SwitchCoroutineStatus::failed;
            state_ = kFinishedState;
            error_code_ = code;
            error_ = std::move(error);
            trace(SwitchCoroutineTraceEvent::fail);
            run_compensations();
        }

        void cancel(std::string error = "canceled")
        {
            status_ = SwitchCoroutineStatus::failed;
            state_ = kFinishedState;
            error_code_ = SwitchCoroutineError::canceled;
            error_ = std::move(error);
            trace(SwitchCoroutineTraceEvent::cancel);
            run_compensations();
        }

        void timeout(std::string error = "timed out")
        {
            status_ = SwitchCoroutineStatus::failed;
            state_ = kFinishedState;
            error_code_ = SwitchCoroutineError::timed_out;
            error_ = std::move(error);
            trace(SwitchCoroutineTraceEvent::timeout);
            run_compensations();
        }

        const std::string &error() const noexcept
        {
            return error_;
        }

        SwitchCoroutineError error_code() const noexcept
        {
            return error_code_;
        }

        bool canceled() const noexcept
        {
            return error_code_ == SwitchCoroutineError::canceled;
        }

        bool timed_out() const noexcept
        {
            return error_code_ == SwitchCoroutineError::timed_out;
        }

        void add_compensation(Compensation compensation)
        {
            if (compensation) {
                compensations_.push_back(std::move(compensation));
            }
        }

        void clear_compensations()
        {
            compensations_.clear();
        }

        void set_trace_hook(TraceHook hook)
        {
            trace_hook_ = std::move(hook);
        }

        void trace(SwitchCoroutineTraceEvent event, std::string_view message = {}) const
        {
            if (trace_hook_) {
                trace_hook_(SwitchCoroutineTrace{ event, state_, error_code_, name_, message });
            }
        }

        void mark_resume() const
        {
            trace(SwitchCoroutineTraceEvent::resume);
        }

        void mark_suspend(int next_state)
        {
            state_ = next_state;
            trace(SwitchCoroutineTraceEvent::suspend);
        }

        void reset()
        {
            state_ = 0;
            status_ = SwitchCoroutineStatus::pending;
            error_code_ = SwitchCoroutineError::none;
            error_.clear();
            compensations_.clear();
        }

    private:
        static constexpr int kFinishedState = -1;

        int state_ = 0;
        SwitchCoroutineStatus status_ = SwitchCoroutineStatus::pending;
        SwitchCoroutineError error_code_ = SwitchCoroutineError::none;
        std::string name_;
        std::string error_;
        std::vector<Compensation> compensations_;
        TraceHook trace_hook_;

        void run_compensations()
        {
            while (!compensations_.empty()) {
                auto compensation = std::move(compensations_.back());
                compensations_.pop_back();
                trace(SwitchCoroutineTraceEvent::compensation);
                if (compensation) {
                    compensation();
                }
            }
        }
    };

    template<typename T>
    class SwitchCoroutineResult : public SwitchCoroutineContext
    {
    public:
        bool has_value() const noexcept
        {
            return value_.has_value();
        }

        const T &value() const &
        {
            return *value_;
        }

        T &value() &
        {
            return *value_;
        }

        T &&value() &&
        {
            return std::move(*value_);
        }

        template<typename U>
        void set_value(U &&value)
        {
            value_.emplace(std::forward<U>(value));
            complete();
        }

        void reset()
        {
            SwitchCoroutineContext::reset();
            value_.reset();
        }

    private:
        std::optional<T> value_;
    };

    template<>
    class SwitchCoroutineResult<void> : public SwitchCoroutineContext
    {
    public:
        void set_value() noexcept
        {
            complete();
        }
    };
}

#define YUAN_SWITCH_COROUTINE_BEGIN(ctx) \
    if ((ctx).finished()) { return (ctx).status(); } \
    (ctx).mark_resume(); \
    switch ((ctx).state()) { case 0:

#define YUAN_SWITCH_COROUTINE_SUSPEND(ctx, next_state) \
    do { \
        (ctx).mark_suspend((next_state)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::pending; \
        case (next_state):; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_WAIT_UNTIL(ctx, next_state, condition) \
    do { \
        (ctx).mark_suspend((next_state)); \
        case (next_state): \
        if (!(condition)) { \
            return ::yuan::game_coroutine::SwitchCoroutineStatus::pending; \
        } \
    } while (false)

#define YUAN_SWITCH_COROUTINE_FAIL(ctx, message) \
    do { \
        (ctx).fail((message)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::failed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_FAIL_CODE(ctx, code, message) \
    do { \
        (ctx).fail((message), (code)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::failed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_CANCEL(ctx, message) \
    do { \
        (ctx).cancel((message)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::failed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_TIMEOUT(ctx, message) \
    do { \
        (ctx).timeout((message)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::failed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_WAIT_CHILD(ctx, next_state, child_expr) \
    do { \
        (ctx).mark_suspend((next_state)); \
        case (next_state): { \
            auto _yuan_child_status = (child_expr); \
            if (_yuan_child_status == ::yuan::game_coroutine::SwitchCoroutineStatus::pending) { \
                return ::yuan::game_coroutine::SwitchCoroutineStatus::pending; \
            } \
            if (_yuan_child_status == ::yuan::game_coroutine::SwitchCoroutineStatus::failed) { \
                (ctx).fail("child coroutine failed", ::yuan::game_coroutine::SwitchCoroutineError::external_error); \
                return ::yuan::game_coroutine::SwitchCoroutineStatus::failed; \
            } \
        } \
    } while (false)

#define YUAN_SWITCH_COROUTINE_COMPENSATE(ctx, fn) \
    do { \
        (ctx).add_compensation((fn)); \
    } while (false)

#define YUAN_SWITCH_COROUTINE_RETURN(ctx, value) \
    do { \
        (ctx).set_value((value)); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::completed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_RETURN_VOID(ctx) \
    do { \
        (ctx).set_value(); \
        return ::yuan::game_coroutine::SwitchCoroutineStatus::completed; \
    } while (false)

#define YUAN_SWITCH_COROUTINE_END(ctx) \
    } \
    (ctx).complete(); \
    return ::yuan::game_coroutine::SwitchCoroutineStatus::completed

#endif
