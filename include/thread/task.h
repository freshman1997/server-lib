#ifndef __TASK_H__
#define __TASK_H__

namespace thread 
{
    class Task 
    {
    public:
        Task(bool valid) : valid_(valid) {}
        void cancel_task() { valid_ = false; }
        void enable_task() { valid_ = true; }
        bool is_valid() const { return valid_; }

        void run() 
        {
            if (!valid_) return;

            run_internal();
        }

    protected:
        virtual void run_internal() = 0;

    private:
        bool valid_;
    };
}

#endif
