#include "plugin_service_registry_adapter.h"

#include "logger.h"
#include "plugin/plugin_context.h"

#include <utility>

namespace yuan::app
{

    namespace
    {

        bool validate_plugin_service_descriptor(const std::string &plugin_name,
                                                const plugin::PluginServiceDescriptor &descriptor)
        {
            if (plugin_name.empty() ||
                descriptor.name.empty() ||
                descriptor.type_name.empty() ||
                descriptor.contract_id.empty() ||
                descriptor.contract_version <= 0) {
                return false;
            }

            return descriptor.plugin_name.empty() || descriptor.plugin_name == plugin_name;
        }

    } // namespace

    bool PluginServiceRegistryAdapter::register_service(const std::string & plugin_name,
                                                        const plugin::PluginServiceDescriptor & descriptor,
                                                        std::any service)
    {
        if (!validate_plugin_service_descriptor(plugin_name, descriptor) || !service.has_value()) {
            return false;
        }

        ServiceEntry entry;
        entry.descriptor = descriptor;
        entry.descriptor.plugin_name = plugin_name;
        entry.instance = std::move(service);

        if (auto managed = std::any_cast<std::shared_ptr<plugin::PluginService> >(&entry.instance)) {
            entry.managed_service = *managed;
            entry.descriptor.managed_lifecycle = static_cast<bool>(entry.managed_service);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (const auto existing = services_.find(entry.descriptor.name); existing != services_.end()) {
            const auto &existing_descriptor = existing->second.descriptor;
            if (existing_descriptor.plugin_name != entry.descriptor.plugin_name ||
                existing_descriptor.type_name != entry.descriptor.type_name ||
                existing_descriptor.contract_id != entry.descriptor.contract_id ||
                existing_descriptor.contract_version != entry.descriptor.contract_version) {
                return false;
            }
            return false;
        }

        services_.emplace(entry.descriptor.name, std::move(entry));
        plugin_services_[plugin_name].push_back(descriptor.name);
        return true;
    }

    void PluginServiceRegistryAdapter::unregister_plugin_services(const std::string & plugin_name)
    {
        stop_plugin_services(plugin_name);

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = plugin_services_.find(plugin_name);
        if (it == plugin_services_.end()) {
            return;
        }

        for (const auto &name : it->second) {
            services_.erase(name);
        }
        plugin_services_.erase(it);
    }

    std::any PluginServiceRegistryAdapter::find_service(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(name);
        if (it == services_.end()) {
            return {};
        }
        return it->second.instance;
    }

    bool PluginServiceRegistryAdapter::describe_service(const std::string & name,
                                                        plugin::PluginServiceDescriptor & descriptor) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = services_.find(name);
        if (it == services_.end()) {
            return false;
        }
        descriptor = it->second.descriptor;
        return true;
    }

    std::vector<plugin::PluginServiceDescriptor> PluginServiceRegistryAdapter::list_services() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<plugin::PluginServiceDescriptor> result;
        result.reserve(services_.size());
        for (const auto & [
                              _,
                              entry
                          ] : services_) {
            result.push_back(entry.descriptor);
        }
        return result;
    }

    bool PluginServiceRegistryAdapter::has_service(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return services_.count(name) > 0;
    }

    std::vector<plugin::PluginServiceDescriptor> PluginServiceRegistryAdapter::list_public_services() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<plugin::PluginServiceDescriptor> result;
        result.reserve(services_.size());
        for (const auto & [
                              _,
                              entry
                          ] : services_) {
            if (entry.descriptor.visibility == plugin::PluginServiceVisibility::public_) {
                result.push_back(entry.descriptor);
            }
        }
        return result;
    }

    bool PluginServiceRegistryAdapter::init_plugin_services(const std::string & plugin_name,
                                                            const plugin::PluginContext & context)
    {
        std::vector<std::pair<std::string, std::shared_ptr<plugin::PluginService> > > services;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = plugin_services_.find(plugin_name);
            if (it == plugin_services_.end()) {
                return true;
            }

            services.reserve(it->second.size());
            for (const auto &name : it->second) {
                auto service_it = services_.find(name);
                if (service_it == services_.end() ||
                    !service_it->second.managed_service ||
                    service_it->second.initialized) {
                    continue;
                }
                services.emplace_back(name, service_it->second.managed_service);
            }
        }

        for (const auto & [
                              name,
                              service
                          ] : services) {
            try
            {
                if (!service->init(context)) {
                    LOG_ERROR("plugin service '{}' init failed for plugin '{}'", name, plugin_name);
                    stop_plugin_services(plugin_name);
                    return false;
                }
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("plugin service '{}' init error for plugin '{}': {}", name, plugin_name, e.what());
                stop_plugin_services(plugin_name);
                return false;
            }
            catch (...)
            {
                LOG_ERROR("plugin service '{}' init unknown error for plugin '{}'", name, plugin_name);
                stop_plugin_services(plugin_name);
                return false;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto service_it = services_.find(name);
            if (service_it != services_.end()) {
                service_it->second.initialized = true;
            }
        }

        return true;
    }

    void PluginServiceRegistryAdapter::start_plugin_services(const std::string & plugin_name)
    {
        std::vector<std::pair<std::string, std::shared_ptr<plugin::PluginService> > > services;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = plugin_services_.find(plugin_name);
            if (it == plugin_services_.end()) {
                return;
            }

            services.reserve(it->second.size());
            for (const auto &name : it->second) {
                auto service_it = services_.find(name);
                if (service_it == services_.end() ||
                    !service_it->second.managed_service ||
                    !service_it->second.initialized ||
                    service_it->second.running) {
                    continue;
                }
                services.emplace_back(name, service_it->second.managed_service);
            }
        }

        for (const auto & [
                              name,
                              service
                          ] : services) {
            try
            {
                service->start();
                std::lock_guard<std::mutex> lock(mutex_);
                auto service_it = services_.find(name);
                if (service_it != services_.end()) {
                    service_it->second.running = true;
                }
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("plugin service '{}' start error for plugin '{}': {}", name, plugin_name, e.what());
            }
            catch (...)
            {
                LOG_ERROR("plugin service '{}' start unknown error for plugin '{}'", name, plugin_name);
            }
        }
    }

    void PluginServiceRegistryAdapter::stop_plugin_services(const std::string & plugin_name)
    {
        std::vector<std::pair<std::string, std::shared_ptr<plugin::PluginService> > > services;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = plugin_services_.find(plugin_name);
            if (it == plugin_services_.end()) {
                return;
            }

            services.reserve(it->second.size());
            for (const auto &name : it->second) {
                auto service_it = services_.find(name);
                if (service_it == services_.end() ||
                    !service_it->second.managed_service ||
                    !service_it->second.initialized) {
                    continue;
                }
                services.emplace_back(name, service_it->second.managed_service);
                service_it->second.running = false;
                service_it->second.initialized = false;
            }
        }

        for (auto rit = services.rbegin(); rit != services.rend(); ++rit) {
            try
            {
                rit->second->stop();
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("plugin service '{}' stop error for plugin '{}': {}", rit->first, plugin_name, e.what());
            }
            catch (...)
            {
                LOG_ERROR("plugin service '{}' stop unknown error for plugin '{}'", rit->first, plugin_name);
            }
        }
    }

    void PluginServiceRegistryAdapter::stop_all_plugin_services()
    {
        std::vector<std::string> plugins;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            plugins.reserve(plugin_services_.size());
            for (const auto & [
                                  plugin_name,
                                  _
                              ] : plugin_services_) {
                plugins.push_back(plugin_name);
            }
        }

        for (auto rit = plugins.rbegin(); rit != plugins.rend(); ++rit) {
            stop_plugin_services(*rit);
        }
    }

} // namespace yuan::app
