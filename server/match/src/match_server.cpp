#include "match_server.h"
#include "logger/include/logger_factory.h"
#include "logger/include/log.h"
#include "core/core/include/timer/wheel_timer_manager.h"
#include "core/core/include/timer/timer_task.h"
#include <chrono>
#include <iostream>

namespace match
{
    using namespace yuan;

    MatchServer::MatchServer()
        : match_pool_(std::make_unique<MatchPool>())
        , running_(false)
        , match_interval_ms_(1000)
        , monitor_interval_ms_(5000)
    {
    }

    MatchServer::~MatchServer()
    {
        stop();
    }

    bool MatchServer::init(const std::string& config_path)
    {
        // 初始化日志
        auto factory = log::LoggerFactory::get_instance();
        factory->init();
        logger_ = factory->get_logger(log::LoggerType::console_);

        if (!logger_)
        {
            std::cerr << "Failed to initialize logger" << std::endl;
            return false;
        }

        // 初始化匹配池
        if (!match_pool_->init(config_path))
        {
            logger_->log_fmt(log::Logger::Level::error,
                "Failed to initialize match pool with config: {}", config_path);
            return false;
        }

        // 设置匹配回调
        match_pool_->set_match_callback(
            [this](const MatchResult& result) {
                this->on_match_success(result);
            });

        logger_->log_fmt(log::Logger::Level::info, "Match server initialized successfully");
        return true;
    }

    bool MatchServer::start()
    {
        if (running_.load())
        {
            return false;
        }

        running_.store(true);
        logger_->log_fmt(log::Logger::Level::info, "Match server started");
        return true;
    }

    void MatchServer::stop()
    {
        if (!running_.load())
        {
            return;
        }

        running_.store(false);
        logger_->log_fmt(log::Logger::Level::info, "Match server stopped");
    }

    void MatchServer::run()
    {
        if (!start())
        {
            return;
        }

        // 创建定时器管理器
        timer::WheelTimerManager timer_manager;

        // 添加匹配检查定时器
        class MatchTask : public timer::TimerTask
        {
        public:
            MatchTask(MatchServer* server) : server_(server) {}
            void on_timer(timer::Timer*) override { server_->on_match_tick(); }
        private:
            MatchServer* server_;
        };

        timer_manager.interval(match_interval_ms_, match_interval_ms_,
            new MatchTask(this), -1);  // 立即执行，周期执行

        // 添加监控定时器
        class MonitorTask : public timer::TimerTask
        {
        public:
            MonitorTask(MatchServer* server) : server_(server) {}
            void on_timer(timer::Timer*) override { server_->on_monitor_tick(); }
        private:
            MatchServer* server_;
        };

        timer_manager.interval(monitor_interval_ms_, monitor_interval_ms_,
            new MonitorTask(this), -1); // 立即执行，周期执行

        logger_->log_fmt(log::Logger::Level::info,
            "Match server main loop started (match_interval={}ms, monitor_interval={}ms)",
            match_interval_ms_, monitor_interval_ms_);

        // 主循环
        while (running_.load())
        {
            timer_manager.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::shared_ptr<MatchNode> MatchServer::create_node(uint64_t player_id, uint32_t score,
                                                         const std::string& extra_data)
    {
        auto node = match_pool_->create_node(player_id, score, extra_data);
        if (node)
        {
            logger_->log_fmt(log::Logger::Level::info,
                "Created node for player {} (score={})", player_id, score);
        }
        return node;
    }

    bool MatchServer::add_player_to_node(uint64_t node_id, uint64_t player_id,
                                          uint32_t score, const std::string& extra_data)
    {
        if (match_pool_->add_player_to_node(node_id, player_id, score, extra_data))
        {
            logger_->log_fmt(log::Logger::Level::info,
                "Player {} joined node {} (score={})", player_id, node_id, score);
            return true;
        }
        return false;
    }

    bool MatchServer::start_match(uint64_t node_id, GameMode mode)
    {
        if (match_pool_->start_match(node_id, mode))
        {
            logger_->log_fmt(log::Logger::Level::info,
                "Node {} started matching (mode={})", node_id, static_cast<int>(mode));
            return true;
        }
        return false;
    }

    bool MatchServer::cancel_match(uint64_t node_id)
    {
        if (match_pool_->cancel_match(node_id))
        {
            logger_->log_fmt(log::Logger::Level::info,
                "Node {} cancelled match", node_id);
            return true;
        }
        return false;
    }

    bool MatchServer::player_offline(uint64_t player_id)
    {
        if (match_pool_->player_offline(player_id))
        {
            logger_->log_fmt(log::Logger::Level::info,
                "Player {} offline, removed from match", player_id);
            return true;
        }
        return false;
    }

    std::shared_ptr<MatchNode> MatchServer::get_node(uint64_t node_id) const
    {
        return match_pool_->get_node(node_id);
    }

    std::string MatchServer::get_statistics() const
    {
        return match_pool_->get_statistics();
    }

    void MatchServer::on_match_success(const MatchResult& result)
    {
        if (!result.success)
        {
            return;
        }

        std::string node_ids;
        uint32_t total_players = 0;
        for (const auto& node : result.matched_nodes)
        {
            if (!node_ids.empty()) node_ids += ", ";
            node_ids += std::to_string(node->get_node_id());
            total_players += static_cast<uint32_t>(node->get_player_count());
        }

        logger_->log_fmt(log::Logger::Level::error,
            "Match success! Room: {}, Mode: {}, Players: {}, Nodes: [{}]",
            result.room_id, static_cast<int>(result.mode), total_players, node_ids);
    }

    void MatchServer::on_match_tick()
    {
        match_pool_->do_match();
    }

    void MatchServer::on_monitor_tick()
    {
        std::string stats = get_statistics();
        logger_->log_fmt(log::Logger::Level::info, "Match pool statistics: {}", stats);
    }
}
