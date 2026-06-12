#ifndef YUAN_GAME_BASE_SERVICE_H
#define YUAN_GAME_BASE_SERVICE_H

#include "game_base/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace yuan::game_base
{
    class IService
    {
    public:
        virtual ~IService() = default;

        [[nodiscard]] virtual std::string_view name() const = 0;
        [[nodiscard]] virtual ServiceState state() const = 0;
        virtual bool start() = 0;
        virtual void stop() = 0;
        virtual void tick(Milliseconds delta) = 0;
    };

    class ServiceBase : public IService
    {
    public:
        explicit ServiceBase(std::string service_name)
            : name_(std::move(service_name))
        {
        }

        [[nodiscard]] std::string_view name() const override
        {
            return name_;
        }

        [[nodiscard]] ServiceState state() const override
        {
            return state_;
        }

        bool start() override
        {
            if (state_ == ServiceState::running) {
                return true;
            }
            state_ = ServiceState::starting;
            if (!on_start()) {
                state_ = ServiceState::failed;
                return false;
            }
            state_ = ServiceState::running;
            return true;
        }

        void stop() override
        {
            if (state_ == ServiceState::stopped) {
                return;
            }
            state_ = ServiceState::stopping;
            on_stop();
            state_ = ServiceState::stopped;
        }

        void tick(Milliseconds delta) override
        {
            if (state_ == ServiceState::running) {
                on_tick(delta);
            }
        }

    protected:
        virtual bool on_start()
        {
            return true;
        }

        virtual void on_stop()
        {
        }

        virtual void on_tick(Milliseconds)
        {
        }

    private:
        std::string name_;
        ServiceState state_ = ServiceState::created;
    };

    class ServiceHost
    {
    public:
        bool add(std::shared_ptr<IService> service)
        {
            if (!service) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            services_.push_back(std::move(service));
            return true;
        }

        bool start_all()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &service : services_) {
                if (!service->start()) {
                    return false;
                }
            }
            return true;
        }

        void stop_all()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
                (*it)->stop();
            }
        }

        void tick_all(Milliseconds delta)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &service : services_) {
                service->tick(delta);
            }
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return services_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::shared_ptr<IService>> services_;
    };
}

#endif
