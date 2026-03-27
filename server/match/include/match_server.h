#ifndef __MATCH_SERVER_H__
#define __MATCH_SERVER_H__

#include <memory>
#include <string>
#include <atomic>
#include "match_pool.h"

namespace yuan::log { class Logger; }
namespace yuan::timer { class TimerManager; }

namespace match
{
    // 匹配服务器
    class MatchServer
    {
    public:
        MatchServer();
        ~MatchServer();

        // 初始化服务器
        bool init(const std::string& config_path);

        // 启动服务器
        bool start();

        // 停止服务器
        void stop();

        // 主循环
        void run();

        // 创建匹配节点（单人）
        std::shared_ptr<MatchNode> create_node(uint64_t player_id, uint32_t score,
                                                const std::string& extra_data = "");

        // 添加玩家到已有节点（组队）
        bool add_player_to_node(uint64_t node_id, uint64_t player_id, 
                               uint32_t score, const std::string& extra_data = "");

        // 开始匹配
        bool start_match(uint64_t node_id, GameMode mode);

        // 取消匹配
        bool cancel_match(uint64_t node_id);

        // 玩家下线
        bool player_offline(uint64_t player_id);

        // 获取节点
        std::shared_ptr<MatchNode> get_node(uint64_t node_id) const;

        // 获取统计信息
        std::string get_statistics() const;

    private:
        // 匹配成功回调
        void on_match_success(const MatchResult& result);

        // 定时检查匹配
        void on_match_tick();

        // 监控任务
        void on_monitor_tick();

    private:
        std::unique_ptr<MatchPool> match_pool_;
        std::shared_ptr<yuan::log::Logger> logger_;
        std::atomic<bool> running_;
        uint32_t match_interval_ms_;
        uint32_t monitor_interval_ms_;
    };
}

#endif
