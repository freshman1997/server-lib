#include "match_server.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <windows.h>

using namespace match;

std::atomic<bool> g_running(true);

void signal_handler(int signal)
{
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running.store(false);
}

void print_usage()
{
    std::cout << "Match Server Usage:" << std::endl;
    std::cout << "  Commands (stdin):" << std::endl;
    std::cout << "    create <player_id> <score>       - Create a match node for player" << std::endl;
    std::cout << "    join <node_id> <player_id> <score> - Add player to existing node (team)" << std::endl;
    std::cout << "    start <node_id> <mode>          - Start matching for node" << std::endl;
    std::cout << "    cancel <node_id>                - Cancel matching" << std::endl;
    std::cout << "    history                         - Show command history" << std::endl;
    std::cout << "    stats                           - Show statistics" << std::endl;
    std::cout << "    quit                            - Shutdown server" << std::endl;
    std::cout << std::endl;
    std::cout << "  Keys:" << std::endl;
    std::cout << "    Up/Down Arrow   - Navigate command history" << std::endl;
    std::cout << std::endl;
    std::cout << "  Modes:" << std::endl;
    std::cout << "    0 - 1v1 Normal (score: creation timestamp in seconds)" << std::endl;
    std::cout << "    1 - 2v2 Ranked (score: ranking points)" << std::endl;
    std::cout << "    2 - 5v5 Team 1 (score: tier)" << std::endl;
    std::cout << "    3 - 5v5 Team 2 (score: tier)" << std::endl;
    std::cout << "    4 - 5v5 Team 3 (score: tier)" << std::endl;
    std::cout << std::endl;
    std::cout << "  Example:" << std::endl;
    std::cout << "    create 1001 500     - Create node for player 1001 with score 500" << std::endl;
    std::cout << "    start 1001 1        - Start 2v2 ranked match for node 1001" << std::endl;
}

// 命令历史管理器（类似 bash 行为）
class CommandHistory
{
public:
    static const size_t MAX_HISTORY = 100;

    void add(const std::string& cmd)
    {
        if (cmd.empty()) return;
        // 避免重复添加相同的连续命令
        if (!history_.empty() && history_.back() == cmd) return;
        
        history_.push_back(cmd);
        if (history_.size() > MAX_HISTORY)
        {
            history_.erase(history_.begin());
        }
        // 重置：下一次浏览从最新的历史开始
        current_index_ = static_cast<int>(history_.size());
        saved_line_.clear();
    }

    // 开始浏览历史前，保存当前正在输入的内容
    void begin_browse(const std::string& current_line)
    {
        saved_line_ = current_line;
    }

    // 上箭头：向更旧的历史移动
    std::string get_previous()
    {
        if (history_.empty()) return "";
        if (current_index_ > 0)
        {
            current_index_--;
        }
        return history_[current_index_];
    }

    // 下箭头：向更新的历史移动
    std::string get_next()
    {
        if (current_index_ < static_cast<int>(history_.size()) - 1)
        {
            current_index_++;
            return history_[current_index_];
        }
        else
        {
            // 已到最新，返回之前保存的输入内容
            current_index_ = static_cast<int>(history_.size());
            return saved_line_;
        }
    }

    const std::vector<std::string>& get_all() const { return history_; }

private:
    std::vector<std::string> history_;
    int current_index_ = 0;
    std::string saved_line_;  // 浏览历史前保存的当前输入
};

// 重绘当前输入行
void redraw_line(HANDLE hStdout, const std::string& line, size_t cursor_pos)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hStdout, &csbi);
    
    // 计算行起始位置（考虑可能的换行）
    SHORT prompt_len = 2;
    SHORT total_len = static_cast<SHORT>(prompt_len + line.length());
    
    // 清除从行首到足够长的区域
    COORD start_pos = {0, csbi.dwCursorPosition.Y};
    
    // 如果内容超过一行宽度，可能需要清除多行
    SHORT console_width = csbi.dwSize.X;
    int lines_needed = (total_len + console_width - 1) / console_width;
    
    DWORD written = 0;
    std::string clear_str(console_width * lines_needed, ' ');
    SetConsoleCursorPosition(hStdout, start_pos);
    WriteConsole(hStdout, clear_str.c_str(), static_cast<DWORD>(clear_str.length()), &written, nullptr);
    SetConsoleCursorPosition(hStdout, start_pos);
    
    // 重新输出内容
    WriteConsole(hStdout, "> ", 2, &written, nullptr);
    WriteConsole(hStdout, line.c_str(), static_cast<DWORD>(line.length()), &written, nullptr);
    
    // 设置光标位置
    COORD cursor_coord;
    cursor_coord.Y = start_pos.Y + static_cast<SHORT>((prompt_len + cursor_pos) / console_width);
    cursor_coord.X = (prompt_len + static_cast<SHORT>(cursor_pos)) % console_width;
    SetConsoleCursorPosition(hStdout, cursor_coord);
}

// 从控制台读取一行，支持上下箭头切换历史和粘贴
std::string read_line_with_history(CommandHistory& history)
{
    static const HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    static const HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    std::string line;
    DWORD chars_written = 0;

    // 显示提示符
    WriteConsole(hStdout, "> ", 2, &chars_written, nullptr);

    // 保存原始控制台模式
    DWORD old_mode = 0;
    GetConsoleMode(hStdin, &old_mode);
    
    // 使用原始模式处理按键
    DWORD new_mode = old_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hStdin, new_mode);

    size_t cursor_pos = 0;
    bool done = false;
    bool browsing = false;

    while (!done && g_running.load())
    {
        INPUT_RECORD record;
        DWORD count = 0;

        if (!ReadConsoleInput(hStdin, &record, 1, &count))
        {
            break;
        }

        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
        {
            continue;
        }

        KEY_EVENT_RECORD& key = record.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;
        DWORD ctrl = key.dwControlKeyState;

        if (vk == VK_RETURN)
        {
            WriteConsole(hStdout, "\r\n", 2, &chars_written, nullptr);
            done = true;
        }
        else if (vk == VK_UP)
        {
            if (!browsing)
            {
                history.begin_browse(line);
                browsing = true;
            }
            line = history.get_previous();
            cursor_pos = line.length();
            redraw_line(hStdout, line, cursor_pos);
        }
        else if (vk == VK_DOWN)
        {
            if (!browsing)
            {
                history.begin_browse(line);
                browsing = true;
            }
            line = history.get_next();
            cursor_pos = line.length();
            redraw_line(hStdout, line, cursor_pos);
        }
        else if (vk == VK_LEFT)
        {
            if (cursor_pos > 0)
            {
                cursor_pos--;
                redraw_line(hStdout, line, cursor_pos);
            }
        }
        else if (vk == VK_RIGHT)
        {
            if (cursor_pos < line.length())
            {
                cursor_pos++;
                redraw_line(hStdout, line, cursor_pos);
            }
        }
        else if (vk == VK_HOME)
        {
            cursor_pos = 0;
            redraw_line(hStdout, line, cursor_pos);
        }
        else if (vk == VK_END)
        {
            cursor_pos = line.length();
            redraw_line(hStdout, line, cursor_pos);
        }
        else if (vk == VK_BACK)
        {
            if (cursor_pos > 0)
            {
                line.erase(cursor_pos - 1, 1);
                cursor_pos--;
                redraw_line(hStdout, line, cursor_pos);
            }
        }
        else if (vk == VK_DELETE)
        {
            if (cursor_pos < line.length())
            {
                line.erase(cursor_pos, 1);
                redraw_line(hStdout, line, cursor_pos);
            }
        }
        else if (vk == 'V' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
        {
            // Ctrl+V 粘贴
            browsing = false;
            
            if (OpenClipboard(nullptr))
            {
                HANDLE hData = GetClipboardData(CF_TEXT);
                if (hData)
                {
                    char* pszText = static_cast<char*>(GlobalLock(hData));
                    if (pszText)
                    {
                        std::string paste_text(pszText);
                        // 移除换行符
                        paste_text.erase(std::remove(paste_text.begin(), paste_text.end(), '\r'), paste_text.end());
                        paste_text.erase(std::remove(paste_text.begin(), paste_text.end(), '\n'), paste_text.end());
                        
                        line.insert(cursor_pos, paste_text);
                        cursor_pos += paste_text.length();
                        redraw_line(hStdout, line, cursor_pos);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
        }
        else if (key.uChar.AsciiChar != 0)
        {
            char ch = key.uChar.AsciiChar;
            // 只接受可打印字符（不包括控制字符）
            if (ch >= 32 && ch != 127)
            {
                browsing = false;
                line.insert(cursor_pos, 1, ch);
                cursor_pos++;
                redraw_line(hStdout, line, cursor_pos);
            }
        }
    }

    // 恢复控制台模式
    SetConsoleMode(hStdin, old_mode);

    return line;
}

void process_command(MatchServer& server, const std::string& line, CommandHistory& history)
{
    if (line.empty())
    {
        return;
    }

    history.add(line);

    // 解析命令
    if (line == "quit" || line == "exit")
    {
        g_running.store(false);
    }
    else if (line == "stats")
    {
        std::cout << server.get_statistics() << std::endl;
    }
    else if (line == "help")
    {
        print_usage();
    }
    else if (line == "history")
    {
        const auto& cmds = history.get_all();
        std::cout << "Command History (" << cmds.size() << " commands):" << std::endl;
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            std::cout << "  " << (i + 1) << ": " << cmds[i] << std::endl;
        }
    }
    else if (line.size() >= 6 && line.substr(0, 6) == "create")
    {
        uint64_t player_id = 0;
        uint32_t score = 0;

        if (sscanf(line.c_str(), "create %llu %u", &player_id, &score) == 2)
        {
            auto node = server.create_node(player_id, score);
            if (node)
            {
                std::cout << "Created node " << node->get_node_id()
                          << " for player " << player_id
                          << " (score=" << score << ")" << std::endl;
            }
            else
            {
                std::cout << "Failed to create node (player already exists?)" << std::endl;
            }
        }
        else
        {
            std::cout << "Invalid command. Usage: create <player_id> <score>" << std::endl;
        }
    }
    else if (line.size() >= 4 && line.substr(0, 4) == "join")
    {
        uint64_t node_id = 0, player_id = 0;
        uint32_t score = 0;

        if (sscanf(line.c_str(), "join %llu %llu %u", &node_id, &player_id, &score) == 3)
        {
            if (server.add_player_to_node(node_id, player_id, score))
            {
                std::cout << "Player " << player_id << " joined node " << node_id << std::endl;
            }
            else
            {
                std::cout << "Failed to join node (node not found or team full?)" << std::endl;
            }
        }
        else
        {
            std::cout << "Invalid command. Usage: join <node_id> <player_id> <score>" << std::endl;
        }
    }
    else if (line.size() >= 5 && line.substr(0, 5) == "start")
    {
        uint64_t node_id = 0;
        int mode = 0;

        if (sscanf(line.c_str(), "start %llu %d", &node_id, &mode) == 2)
        {
            if (server.start_match(node_id, static_cast<GameMode>(mode)))
            {
                std::cout << "Node " << node_id << " started matching (mode=" << mode << ")" << std::endl;
            }
            else
            {
                std::cout << "Failed to start match (node not found or already matching?)" << std::endl;
            }
        }
        else
        {
            std::cout << "Invalid command. Usage: start <node_id> <mode>" << std::endl;
        }
    }
    else if (line.size() >= 6 && line.substr(0, 6) == "cancel")
    {
        uint64_t node_id = 0;
        if (sscanf(line.c_str(), "cancel %llu", &node_id) == 1)
        {
            if (server.cancel_match(node_id))
            {
                std::cout << "Node " << node_id << " cancelled matching" << std::endl;
            }
            else
            {
                std::cout << "Failed to cancel (node not matching?)" << std::endl;
            }
        }
        else
        {
            std::cout << "Invalid command. Usage: cancel <node_id>" << std::endl;
        }
    }
    else
    {
        std::cout << "Unknown command. Type 'help' for usage." << std::endl;
    }
}

int main(int argc, char* argv[])
{
    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_path = "config/match_config.json";
    if (argc > 1)
    {
        config_path = argv[1];
    }

    // 创建并初始化匹配服务器
    MatchServer server;

    std::cout << "Initializing Match Server..." << std::endl;
    if (!server.init(config_path))
    {
        std::cerr << "Failed to initialize match server" << std::endl;
        return 1;
    }

    print_usage();

    // 在独立线程中运行服务器主循环
    std::thread server_thread([&server]() {
        server.run();
    });

    CommandHistory history;

    // 主线程处理用户输入
    while (g_running.load())
    {
        std::string line = read_line_with_history(history);
        if (!g_running.load()) break;
        process_command(server, line, history);
    }

    // 停止服务器
    server.stop();

    if (server_thread.joinable())
    {
        server_thread.join();
    }

    std::cout << "Match Server exited." << std::endl;
    return 0;
}
