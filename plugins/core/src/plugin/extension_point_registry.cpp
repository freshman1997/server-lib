#include "plugin/extension_point_registry.h"

#include <algorithm>

namespace yuan::plugin
{

    bool ExtensionPointRegistry::register_extension_point(const std::string & plugin_name,
                                                          const ExtensionPointDescriptor & descriptor,
                                                          std::any implementation)
    {
        if (descriptor.name.empty() || plugin_name.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        std::size_t idx = entries_.size();
        entries_.push_back(ExtensionPointEntry{
            plugin_name,
            descriptor.name,
            descriptor.type,
            descriptor.contract_id,
            descriptor.contract_version,
            std::move(implementation)
        });

        name_index_[descriptor.name].push_back(idx);

        if (!descriptor.contract_id.empty()) {
            contract_index_[descriptor.contract_id].push_back(idx);
        }

        plugin_index_[plugin_name].push_back(idx);

        return true;
    }

    bool ExtensionPointRegistry::unregister_extension_points(const std::string & plugin_name)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = plugin_index_.find(plugin_name);
        if (it == plugin_index_.end()) {
            return false;
        }

        for (std::size_t idx : it->second) {
            const auto &entry = entries_[idx];

            auto name_it = name_index_.find(entry.extension_point_name);
            if (name_it != name_index_.end()) {
                auto &vec = name_it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), idx), vec.end());
                if (vec.empty()) {
                    name_index_.erase(name_it);
                }
            }

            if (!entry.contract_id.empty()) {
                auto contract_it = contract_index_.find(entry.contract_id);
                if (contract_it != contract_index_.end()) {
                    auto &vec = contract_it->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), idx), vec.end());
                    if (vec.empty()) {
                        contract_index_.erase(contract_it);
                    }
                }
            }

            entries_[idx] = ExtensionPointEntry{};
        }

        plugin_index_.erase(it);
        return true;
    }

    std::vector<const ExtensionPointEntry *> ExtensionPointRegistry::find_by_name(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<const ExtensionPointEntry *> result;
        auto it = name_index_.find(name);
        if (it == name_index_.end()) {
            return result;
        }

        for (std::size_t idx : it->second) {
            if (!entries_[idx].extension_point_name.empty()) {
                result.push_back(&entries_[idx]);
            }
        }
        return result;
    }

    std::vector<const ExtensionPointEntry *> ExtensionPointRegistry::find_by_contract(const std::string & contract_id,
                                                                                      int min_version) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<const ExtensionPointEntry *> result;
        auto it = contract_index_.find(contract_id);
        if (it == contract_index_.end()) {
            return result;
        }

        for (std::size_t idx : it->second) {
            if (entries_[idx].contract_version >= min_version &&
                !entries_[idx].extension_point_name.empty()) {
                result.push_back(&entries_[idx]);
            }
        }
        return result;
    }

    const ExtensionPointEntry *ExtensionPointRegistry::find_one(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = name_index_.find(name);
        if (it == name_index_.end() || it->second.empty()) {
            return nullptr;
        }

        for (std::size_t idx : it->second) {
            if (!entries_[idx].extension_point_name.empty()) {
                return &entries_[idx];
            }
        }
        return nullptr;
    }

    const ExtensionPointEntry *ExtensionPointRegistry::find_best_contract(const std::string & contract_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = contract_index_.find(contract_id);
        if (it == contract_index_.end() || it->second.empty()) {
            return nullptr;
        }

        const ExtensionPointEntry *best = nullptr;
        for (std::size_t idx : it->second) {
            if (entries_[idx].extension_point_name.empty()) {
                continue;
            }
            if (!best || entries_[idx].contract_version > best->contract_version) {
                best = &entries_[idx];
            }
        }
        return best;
    }

    std::vector<ExtensionPointEntry> ExtensionPointRegistry::all_entries() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<ExtensionPointEntry> result;
        result.reserve(entries_.size());
        for (const auto &entry : entries_) {
            if (!entry.extension_point_name.empty()) {
                result.push_back(entry);
            }
        }
        return result;
    }

    std::size_t ExtensionPointRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto &entry : entries_) {
            if (!entry.extension_point_name.empty()) {
                ++count;
            }
        }
        return count;
    }

    void ExtensionPointRegistry::clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        name_index_.clear();
        contract_index_.clear();
        plugin_index_.clear();
    }

} // namespace yuan::plugin
