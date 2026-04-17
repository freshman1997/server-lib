#ifndef __NET_SMB_SMB_PIPE_MANAGER_H__
#define __NET_SMB_SMB_PIPE_MANAGER_H__

#include "protocol/smb2_constants.h"
#include "protocol/smb2_structures.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::net::smb
{
    struct PipeInstance
    {
        uint64_t handle = 0;
        std::string name;
        uint64_t session_id = 0;
    };

    class SmbPipeManager
    {
    public:
        using PipeReadHandler = std::function<std::vector<uint8_t>(const std::string &name, uint64_t handle, uint32_t len)>;
        using PipeWriteHandler = std::function<uint32_t(const std::string &name, uint64_t handle, const uint8_t *data, uint32_t len)>;

        void register_pipe(const std::string &name);
        void register_pipe(const std::string &name, PipeReadHandler read_fn, PipeWriteHandler write_fn);
        void unregister_pipe(const std::string &name);

        bool exists(const std::string &name) const;
        std::vector<std::string> list_pipes() const;

        uint64_t open_pipe(const std::string &name, uint64_t session_id);
        void close_pipe(uint64_t handle);
        PipeInstance *find_pipe(uint64_t handle);

        std::vector<uint8_t> read_pipe(uint64_t handle, uint32_t len);
        uint32_t write_pipe(uint64_t handle, const uint8_t *data, uint32_t len);

        void register_builtin_pipes();

    private:
        struct PipeInfo
        {
            std::string name;
            PipeReadHandler read_fn;
            PipeWriteHandler write_fn;
        };

        mutable std::mutex mutex_;
        std::unordered_map<std::string, PipeInfo> pipes_;
        std::unordered_map<uint64_t, PipeInstance> instances_;
        std::atomic<uint64_t> next_handle_{ 1 };
    };
}
#endif
