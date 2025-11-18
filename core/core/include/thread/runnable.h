#ifndef __TASK_H__
#define __TASK_H__

namespace yuan::thread 
{
    class Runnable 
    {
    public:
        Runnable() : valid_(true) {}
        explicit Runnable(const bool valid) : valid_(valid) {}
        virtual ~Runnable() = default;
        void cancel_task() { valid_ = false; }
        void enable_task() { valid_ = true; }
        bool is_valid() const { return valid_; }

        void run() 
        {
            if (valid_) {
                run_internal();
            }
        }

    protected:
        virtual void run_internal() = 0;

    private:
        bool valid_;
    };
}

#endif
