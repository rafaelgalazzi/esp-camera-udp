#pragma once
#include <iostream>
#include <string>
#include <vector>

// Access under log_mutex while worker threads are running.
class TerminalPanel
{
    bool enabled = false;
    std::vector<std::string> previous;
#ifdef _WIN32
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD original_mode = 0;
#endif
public:
    std::string event = "Waiting for cameras...";
    bool active() const { return enabled; }
    void start(bool json)
    {
#ifdef _WIN32
        if (json)
            return;
        output = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleMode(output, &original_mode) || !GetConsoleScreenBufferInfo(output, &info) ||
            info.srWindow.Right - info.srWindow.Left < 54 || info.srWindow.Bottom - info.srWindow.Top < 17)
            return;
        if (!SetConsoleMode(output, original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return;
        enabled = true;
        std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;
#else
        (void)json;
#endif
    }
    void draw(const std::vector<std::string> &lines)
    {
        if (!enabled)
            return;
        size_t width = 79, height = lines.size();
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(output, &info))
        {
            width = static_cast<size_t>(info.srWindow.Right - info.srWindow.Left);
            height = static_cast<size_t>(info.srWindow.Bottom - info.srWindow.Top);
        }
#endif
        previous.resize(lines.size());
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string line = lines[i].substr(0, width);
            for (auto &c : line)
                if (static_cast<unsigned char>(c) < 32 || c == 127)
                    c = ' ';
            if (i >= height)
            {
                previous[i].clear();
                continue;
            }
            if (line != previous[i])
            {
                std::cout << "\x1b[" << i + 1 << ";1H\x1b[2K" << line;
                previous[i] = line;
            }
        }
        std::cout.flush();
    }
    void stop()
    {
        if (!enabled)
            return;
        std::cout << "\x1b[?25h\x1b[18;1H\n" << std::flush;
#ifdef _WIN32
        SetConsoleMode(output, original_mode);
#endif
        enabled = false;
    }
    ~TerminalPanel() { stop(); }
};
