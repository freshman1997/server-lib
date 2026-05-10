#include "match_server.h"
#include "logger_factory.h"
#include "log.h"
#include "timer/timer_util.hpp"
#include "timer/wheel_timer_manager.h"
#include "timer/timer_task.h"
#include <chrono>
#include <iostream>
#include <thread>

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
        auto factory = log::LoggerFactory::get_instance();
        factory->init();
        logger_ = factory->get_logger(log::LoggerType::console_);

        if (!logger_)
        {
            std::cerr << "Failed to initialize logger" << std::endl;
            return false;
        }

        if (!match_pool_->init(config_path))
        {
            logger_->log_fmt(log::Level::error,
                "Failed to initialize match pool with config: {}", config_path);
            return false;
        }

        match_pool_->set_match_callback(
            [this](const MatchResult& result) {
                this->on_match_success(result);
            });

        logger_->log_fmt(log::Level::info, "Match server initialized successfully");
        return true;
    }

    bool MatchServer::start()
    {
        if (running_.load())
        {
            return false;
        }

        running_.store(true);
        logger_->log_fmt(log::Level::info, "Match server started");
        return true;
    }

    void MatchServer::stop()
    {
        if (!running_.load())
        {
            return;
        }

        running_.store(false);
        logger_->log_fmt(log::Level::info, "Match server stopped");
    }

    void MatchServer::run()
    {
        if (!start())
        {
            return;
        }

        timer::WheelTimerManager timer_manager;

        timer::TimerUtil::build_period_handle(
            &timer_manager,
            match_interval_ms_,
            match_interval_ms_,
            [this]() {
                on_match_tick();
            },
            -1);

        timer::TimerUtil::build_period_handle(
            &timer_manager,
            monitor_interval_ms_,
            monitor_interval_ms_,
            [this]() {
                on_monitor_tick();
            },
            -1);

        logger_->log_fmt(log::Level::info,
            "Match server main loop started (match_interval={}ms, monitor_interval={}ms)",
            match_interval_ms_, monitor_interval_ms_);

        while (running_.load())
        {
            timer_manager.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(timer_manager.get_poll_timeout_ms(50, 1)));
        }
    }

    std::shared_ptr<MatchNode> MatchServer::create_node(uint64_t player_id, uint32_t score,
                                                         const std::string& extra_data)
    {
        auto node = match_pool_->create_node(player_id, score, extra_data);
        if (node)
        {
            logger_->log_fmt(log::Level::info,
                "Created node for player {} (score={})", player_id, score);
        }
        return node;
    }

    bool MatchServer::add_player_to_node(uint64_t node_id, uint64_t player_id,
                                          uint32_t score, const std::string& extra_data)
    {
        if (match_pool_->add_player_to_node(node_id, player_id, score, extra_data))
        {
            logger_->log_fmt(log::Level::info,
                "Player {} joined node {} (score={})", player_id, node_id, score);
            return true;
        }
        return false;
    }

    bool MatchServer::start_match(uint64_t node_id, GameMode mode)
    {
        if (match_pool_->start_match(node_id, mode))
        {
            logger_->log_fmt(log::Level::info,
                "Node {} started matching (mode={})", node_id, static_cast<int>(mode));
            return true;
        }
        return false;
    }

    bool MatchServer::cancel_match(uint64_t node_id)
    {
        if (match_pool_->cancel_match(node_id))
        {
            logger_->log_fmt(log::Level::info,
                "Node {} cancelled match", node_id);
            return true;
        }
        return false;
    }

    bool MatchServer::player_offline(uint64_t player_id)
    {
        if (match_pool_->player_offline(player_id))
        {
            logger_->log_fmt(log::Level::info,
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

        logger_->log_fmt(log::Level::error,
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
        logger_->log_fmt(log::Level::info, "Match pool statistics: {}", stats);
    }
}
