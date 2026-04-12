#include "console_logger.h"
#include "base/time.h"
#include "formatter.h"
#include <cstdarg>
#include <cstdio>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#endif

namespace yuan::log
{

namespace
{

static inline LogItem make_log_item(Level level, const std::string& msg, const std::string& name,
                                    const char* file = nullptr, int line = 0, const char* func = nullptr)
{
    LogItem item;
    item.level = level;
    item.message = msg;
    item.logger_name = name;
    item.line = line;

    const auto now_ms = yuan::base::time::system_now_ms();
    item.timestamp = static_cast<std::time_t>(now_ms / 1000ULL);
    item.milliseconds = now_ms % 1000ULL;

    if (file) item.source_file = file;
    if (func) item.function_name = func;
    return item;
}

#ifdef _WIN32
static std::wstring utf8_to_wide(const std::string& utf8)
{
    if (utf8.empty()) return {};

    const DWORD flags = MB_ERR_INVALID_CHARS;
    int len = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (len <= 0) return L"[encoding_error]";
    }

    std::wstring wstr(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wstr.data(), len);
    return wstr;
}

static bool is_real_console()
{
    static int result = -1;
    if (result >= 0) return result == 1;

    HANDLE h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h_stdout || h_stdout == INVALID_HANDLE_VALUE) {
        result = 0;
        return false;
    }

    DWORD mode = 0;
    result = GetConsoleMode(h_stdout, &mode) ? 1 : 0;
    return result == 1;
}

static bool enable_virtual_terminal()
{
    static int state = -1;
    if (state >= 0) return state == 1;

    HANDLE h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h_stdout || h_stdout == INVALID_HANDLE_VALUE) {
        state = 0;
        return false;
    }

    DWORD mode = 0;
    if (!GetConsoleMode(h_stdout, &mode)) {
        state = 0;
        return false;
    }

    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        state = 1;
        return true;
    }

    state = SetConsoleMode(h_stdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? 1 : 0;
    return state == 1;
}

static std::string strip_ansi_codes(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            while (i < s.size() && s[i] != 'm') ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}
#endif

static void console_output(const std::string& text)
{
#ifdef _WIN32
    if (is_real_console()) {
        auto output = enable_virtual_terminal() ? text : strip_ansi_codes(text);
        const auto wtext = utf8_to_wide(output);
        HANDLE h_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (WriteConsoleW(h_stdout, wtext.data(), static_cast<DWORD>(wtext.size()), &written, nullptr)) {
            return;
        }
    }

    const auto clean = strip_ansi_codes(text);
    if (!clean.empty()) {
        _write(_fileno(stdout), clean.data(), static_cast<unsigned int>(clean.size()));
    }
#else
    std::fwrite(text.data(), 1, text.size(), stdout);
#endif
    std::fflush(stdout);
}

static std::string vformat_message(const char* fmt, va_list args)
{
    char stack_buf[4096];
    va_list args_copy;
    va_copy(args_copy, args);
    const int written = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
    va_end(args_copy);

    if (written < 0) return {};
    if (static_cast<size_t>(written) < sizeof(stack_buf)) {
        return std::string(stack_buf, static_cast<size_t>(written));
    }

    std::vector<char> heap_buf(static_cast<size_t>(written) + 1, '\0');
    va_list args_retry;
    va_copy(args_retry, args);
    std::vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args_retry);
    va_end(args_retry);
    return std::string(heap_buf.data(), static_cast<size_t>(written));
}

} // namespace

ConsoleLogger::ConsoleLogger(const std::string& name)
{
    name_ = name;
    formatter_ = std::make_shared<Formatter>();
}

ConsoleLogger::~ConsoleLogger()
{
    flush();
}

void ConsoleLogger::log(Level level, const char* fmt, ...)
{
    if (level < level_.load(std::memory_order_relaxed)) return;

    va_list args;
    va_start(args, fmt);
    const std::string msg = vformat_message(fmt, args);
    va_end(args);

    log_impl(level, msg);
}

void ConsoleLogger::log_impl(Level level, const std::string& msg)
{
    LogItem item = make_log_item(level, msg, name_);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto color = get_color(level);
    console_output(color + formatted + CLR_CLR + "\n");
}

void ConsoleLogger::log_impl(Level level, const std::string& msg,
                             const char* file, int line, const char* func)
{
    LogItem item = make_log_item(level, msg, name_, file, line, func);
    std::string formatted;
    try {
        formatted = format_log_item(item);
    } catch (...) {
        formatted = "[format_error] " + msg;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto color = get_color(level);
    console_output(color + formatted + CLR_CLR + "\n");
}

void ConsoleLogger::flush()
{
    std::fflush(stdout);
}

} // namespace yuan::log
