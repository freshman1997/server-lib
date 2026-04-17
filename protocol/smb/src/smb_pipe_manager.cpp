#include "smb_pipe_manager.h"
#include <algorithm>

namespace yuan::net::smb
{
    void SmbPipeManager::register_pipe(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PipeInfo info;
        info.name = name;
        info.read_fn = [](const std::string &, uint64_t, uint32_t)->std::vector<uint8_t>
        {
            return {};
        };
        info.write_fn = [](const std::string &, uint64_t, const uint8_t *, uint32_t)->uint32_t
        {
            return 0;
        };
        pipes_[name] = std::move(info);
    }

    void SmbPipeManager::register_pipe(const std::string & name, PipeReadHandler read_fn, PipeWriteHandler write_fn)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PipeInfo info;
        info.name = name;
        info.read_fn = std::move(read_fn);
        info.write_fn = std::move(write_fn);
        pipes_[name] = std::move(info);
    }

    void SmbPipeManager::unregister_pipe(const std::string & name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pipes_.erase(name);
    }

    bool SmbPipeManager::exists(const std::string & name) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pipes_.find(name) != pipes_.end();
    }

    std::vector<std::string> SmbPipeManager::list_pipes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(pipes_.size());
        for (const auto & [
                              name,
                              info
                          ] : pipes_) {
            result.push_back(name);
        }
        return result;
    }

    uint64_t SmbPipeManager::open_pipe(const std::string & name, uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pipes_.find(name);
        if (it == pipes_.end())
            return 0;

        uint64_t handle = next_handle_.fetch_add(1);
        PipeInstance inst;
        inst.handle = handle;
        inst.name = name;
        inst.session_id = session_id;
        instances_[handle] = inst;
        return handle;
    }

    void SmbPipeManager::close_pipe(uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        instances_.erase(handle);
    }

    PipeInstance *SmbPipeManager::find_pipe(uint64_t handle)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(handle);
        if (it != instances_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    std::vector<uint8_t> SmbPipeManager::read_pipe(uint64_t handle, uint32_t len)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iit = instances_.find(handle);
        if (iit == instances_.end())
            return {};

        auto pit = pipes_.find(iit->second.name);
        if (pit == pipes_.end())
            return {};

        return pit->second.read_fn(pit->second.name, handle, len);
    }

    uint32_t SmbPipeManager::write_pipe(uint64_t handle, const uint8_t * data, uint32_t len)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto iit = instances_.find(handle);
        if (iit == instances_.end())
            return 0;

        auto pit = pipes_.find(iit->second.name);
        if (pit == pipes_.end())
            return 0;

        return pit->second.write_fn(pit->second.name, handle, data, len);
    }

    void SmbPipeManager::register_builtin_pipes()
    {
        auto noop_read = [](const std::string &, uint64_t, uint32_t)->std::vector<uint8_t>
        {
            return {};
        };
        auto noop_write = [](const std::string &, uint64_t, const uint8_t *, uint32_t)->uint32_t
        {
            return 0;
        };

        register_pipe("srvsvc", noop_read, noop_write);
        register_pipe("wkssvc", noop_read, noop_write);
        register_pipe("samr", noop_read, noop_write);
        register_pipe("lsarpc", noop_read, noop_write);
        register_pipe("netlogon", noop_read, noop_write);
    }
}
