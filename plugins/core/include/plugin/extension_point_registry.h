#ifndef __YUAN_PLUGIN_EXTENSION_POINT_REGISTRY_H__
#define __YUAN_PLUGIN_EXTENSION_POINT_REGISTRY_H__

#include "plugin/plugin_manifest.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <any>

namespace yuan::plugin
{

    struct ExtensionPointEntry
    {
        std::string plugin_name;
        std::string extension_point_name;
        std::string type;
        std::string contract_id;
        int contract_version = 1;
        std::any implementation;
    };

    class ExtensionPointRegistry
    {
    public:
        bool register_extension_point(const std::string &plugin_name,
                                      const ExtensionPointDescriptor &descriptor,
                                      std::any implementation = {});

        bool unregister_extension_points(const std::string &plugin_name);

        std::vector<const ExtensionPointEntry *> find_by_name(const std::string &name) const;

        std::vector<const ExtensionPointEntry *> find_by_contract(const std::string &contract_id,
                                                                  int min_version = 1) const;

        const ExtensionPointEntry *find_one(const std::string &name) const;

        const ExtensionPointEntry *find_best_contract(const std::string &contract_id) const;

        std::vector<ExtensionPointEntry> all_entries() const;

        std::size_t size() const;

        void clear();

    private:
        mutable std::mutex mutex_;
        std::vector<ExtensionPointEntry> entries_;
        std::unordered_map<std::string, std::vector<std::size_t> > name_index_;
        std::unordered_map<std::string, std::vector<std::size_t> > contract_index_;
        std::unordered_map<std::string, std::vector<std::size_t> > plugin_index_;
    };

} // namespace yuan::plugin

#endif
