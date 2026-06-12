#ifndef YUAN_BASE_ALGORITHM_SCOPE_GUARD_H_
#define YUAN_BASE_ALGORITHM_SCOPE_GUARD_H_

#include <utility>

namespace yuan::base
{
    // ScopeExit 用于作用域退出时自动执行清理逻辑，适合临时状态回滚、文件/句柄
    // 清理、锁外补偿等场景。
    //
    // 用法：
    //   auto guard = yuan::base::make_scope_exit([&] { cleanup(); });
    //   ...
    //   guard.dismiss(); // 如果不再需要退出时执行清理。
    template <typename Fn>
    class ScopeExit
    {
    public:
        explicit ScopeExit(Fn fn) noexcept
            : fn_(std::move(fn))
        {
        }

        ~ScopeExit() noexcept
        {
            if (active_) {
                fn_();
            }
        }

        ScopeExit(const ScopeExit&) = delete;
        ScopeExit& operator=(const ScopeExit&) = delete;

        ScopeExit(ScopeExit&& rhs) noexcept
            : fn_(std::move(rhs.fn_)), active_(rhs.active_)
        {
            rhs.active_ = false;
        }

        ScopeExit& operator=(ScopeExit&&) = delete;

        void dismiss() noexcept
        {
            active_ = false;
        }

    private:
        Fn fn_;
        bool active_ = true;
    };

    template <typename Fn>
    ScopeExit<Fn> make_scope_exit(Fn fn)
    {
        return ScopeExit<Fn>(std::move(fn));
    }
}

#endif // YUAN_BASE_ALGORITHM_SCOPE_GUARD_H_
