#ifndef __YUAN_PLUGIN_PLUGIN_STATE_H__
#define __YUAN_PLUGIN_PLUGIN_STATE_H__

#include <string>

namespace yuan::plugin
{

    enum class PluginState {
        discovered,
        loaded,
        initialized,
        active,
        degraded,
        faulted,
        quarantined,
        stopping,
        stopped,
        unloaded,
    };

    inline const char *to_string(PluginState state)
    {
        switch (state) {
        case PluginState::discovered:
            return "discovered";
        case PluginState::loaded:
            return "loaded";
        case PluginState::initialized:
            return "initialized";
        case PluginState::active:
            return "active";
        case PluginState::degraded:
            return "degraded";
        case PluginState::faulted:
            return "faulted";
        case PluginState::quarantined:
            return "quarantined";
        case PluginState::stopping:
            return "stopping";
        case PluginState::stopped:
            return "stopped";
        case PluginState::unloaded:
            return "unloaded";
        default:
            return "unknown";
        }
    }

    inline bool can_transition(PluginState from, PluginState to)
    {
        switch (from) {
        case PluginState::discovered:
            return to == PluginState::loaded || to == PluginState::unloaded;
        case PluginState::loaded:
            return to == PluginState::initialized || to == PluginState::stopped || to == PluginState::unloaded;
        case PluginState::initialized:
            return to == PluginState::active || to == PluginState::faulted || to == PluginState::stopped || to == PluginState::unloaded;
        case PluginState::active:
            return to == PluginState::degraded || to == PluginState::faulted || to == PluginState::stopping || to == PluginState::stopped;
        case PluginState::degraded:
            return to == PluginState::active || to == PluginState::faulted || to == PluginState::stopping || to == PluginState::stopped;
        case PluginState::faulted:
            return to == PluginState::quarantined || to == PluginState::stopping || to == PluginState::stopped || to == PluginState::degraded;
        case PluginState::quarantined:
            return to == PluginState::stopping || to == PluginState::stopped;
        case PluginState::stopping:
            return to == PluginState::stopped;
        case PluginState::stopped:
            return to == PluginState::unloaded;
        case PluginState::unloaded:
            return false;
        default:
            return false;
        }
    }

    inline bool is_operational(PluginState state)
    {
        return state == PluginState::active || state == PluginState::degraded;
    }

    inline bool is_terminal(PluginState state)
    {
        return state == PluginState::unloaded;
    }

    inline bool accepts_callbacks(PluginState state)
    {
        return state == PluginState::loaded ||
               state == PluginState::initialized ||
               state == PluginState::active ||
               state == PluginState::degraded;
    }

} // namespace yuan::plugin

#endif
