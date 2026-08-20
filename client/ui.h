#pragma once

#include <ncurses.h>
#include <functional>
#include <deque>
#include <mutex>
#include <string>
#include <locale.h>
#include <curses.h>
#include <codecvt>

class cUI {
public:
    cUI() : msg_win_(nullptr), input_win_(nullptr), status_win_(nullptr) {}
    ~cUI() {
        if (msg_win_) delwin(msg_win_);
        if (input_win_) delwin(input_win_);
        if (status_win_) delwin(status_win_);
        endwin();
    }

    bool UIinit() {
        setlocale(LC_ALL, "");   
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(1);
        refresh();

        int h, w;
        getmaxyx(stdscr, h, w);

        int status_h = 1;
        int input_h = 3;
        int msg_h = h - status_h - input_h;

        status_win_ = newwin(status_h, w, 0, 0);
        msg_win_ = newwin(msg_h, w, status_h, 0);
        input_win_ = newwin(input_h, w, status_h + msg_h, 0);

        // 启用滚动
        scrollok(msg_win_, TRUE);
        keypad(input_win_, TRUE);

        // 初始绘制
        werase(status_win_);
        mvwprintw(status_win_, 0, 2, "Status: Connected");
        wrefresh(status_win_);

        box(msg_win_, 0, 0);
        wmove(msg_win_, 1, 2);  // 移动光标到第一行
        wrefresh(msg_win_);

        box(input_win_, 0, 0);
        mvwprintw(input_win_, 1, 2, "> ");
        wmove(input_win_, 1, 4);
        wrefresh(input_win_);

        return true;
    }

    void setStatus(const std::string& text) {
        if (!status_win_) return;
        werase(status_win_);
        mvwprintw(status_win_, 0, 2, "%s", text.c_str());
        wrefresh(status_win_);
    }

    void displayMessage(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(msg);
            if (lines_.size() > MAX_LINES) {
                lines_.pop_front();
            }
        }
        appendMessageToWindow(msg);
    }

    void displaySystem(const std::string& msg) { displayMessage("[系统] " + msg); }
    void displayError(const std::string& msg)   { displayMessage("[错误] " + msg); }

    std::string getInput() {
        werase(input_win_);
        box(input_win_, 0, 0);

        mvwprintw(input_win_, 1, 2, "> ");
        wmove(input_win_, 1, 4);

        noecho();

        std::wstring winput;
        wint_t ch;

        while (true) {
            int ret = wget_wch(input_win_, &ch);

            if (ret == ERR)
                continue;

            if (ret == OK) {
                if (ch == L'\n' || ch == L'\r') {
                    break;
                }

                if (ch == 127 || ch == 8) {
                    if (!winput.empty()) {
                        winput.pop_back();

                        int y, x;
                        getyx(input_win_, y, x);

                        if (x > 4) {
                            wmove(input_win_, y, x - 1);
                            wdelch(input_win_);
                        }
                    }
                }
                else if (ch >= 32) {
                    winput.push_back(static_cast<wchar_t>(ch));

                    wchar_t wc = static_cast<wchar_t>(ch);
                    waddnwstr(input_win_, &wc, 1);
                }
            }
            else if (ret == KEY_CODE_YES) {
                if (ch == KEY_BACKSPACE) {
                    if (!winput.empty()) {
                        winput.pop_back();

                        int y, x;
                        getyx(input_win_, y, x);

                        if (x > 4) {
                            wmove(input_win_, y, x - 1);
                            wdelch(input_win_);
                        }
                    }
                }
            }

            wrefresh(input_win_);
        }

        wrefresh(input_win_);

        if (winput.empty())
            return "";

        size_t len = wcstombs(nullptr, winput.c_str(), 0);

        if (len == static_cast<size_t>(-1))
            return "";

        std::string result(len, '\0');

        wcstombs(&result[0], winput.c_str(), len);

        return result;
    }

    void refresh() {
        wrefresh(status_win_);
        wrefresh(input_win_);
        wrefresh(msg_win_);
    }

    void run(std::function<void(const std::string&)> handler) {
        while (true) {
            refresh();
            std::string input = getInput();
            if (input == "/quit" || input == "exit") break;
            if (!input.empty()) handler(input);
        }
    }

    void clearScreen() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
        if (msg_win_) {
            werase(msg_win_);
            box(msg_win_, 0, 0);
            wmove(msg_win_, 1, 2);
            wrefresh(msg_win_);
        }
    }

private:
    WINDOW* msg_win_;
    WINDOW* input_win_;
    WINDOW* status_win_;
    std::deque<std::string> lines_;
    std::mutex mutex_;
    static const int MAX_LINES = 1000;

    // ✅ 稳定的消息追加函数
    void appendMessageToWindow(const std::string& msg) {
        if (!msg_win_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        int max_y = getmaxy(msg_win_) - 1;
        int max_x = getmaxx(msg_win_) - 4;
        int content_height = max_y - 2;  // 减去上下边框
        
        if (content_height <= 0) return;
        
        // ✅ 获取当前光标位置
        int cur_y, cur_x;
        getyx(msg_win_, cur_y, cur_x);
        
        // ✅ 如果光标在边框或无效位置，重置到第一行
        if (cur_y <= 0 || cur_y >= max_y) {
            cur_y = 1;
            cur_x = 2;
            wmove(msg_win_, cur_y, cur_x);
        }
        
        // ✅ 将消息按行分割
        std::vector<std::string> lines = splitMessage(msg, max_x);
        
        for (const std::string& line : lines) {
            // ✅ 检查是否需要换行或滚动
            if (cur_y >= max_y - 1) {
                // 窗口满了，向上滚动一行
                wscrl(msg_win_, 1);
                // 移动到最后一行
                cur_y = max_y - 1;
                cur_x = 2;
                wmove(msg_win_, cur_y, cur_x);
                // 清空最后一行
                wclrtoeol(msg_win_);
            } else {
                // 检查当前行是否还有空间
                if (cur_x > 2) {
                    // 当前行已有内容，换到下一行
                    cur_y++;
                    cur_x = 2;
                    wmove(msg_win_, cur_y, cur_x);
                }
            }
            
            // 打印消息行
            mvwprintw(msg_win_, cur_y, cur_x, "%s", line.c_str());
            
            // 更新光标位置
            cur_x += line.length();
            wmove(msg_win_, cur_y, cur_x);
        }
        
        wrefresh(msg_win_);
    }
    
    // 将长消息分割成多行
    std::vector<std::string> splitMessage(const std::string& msg, int max_width) {
        std::vector<std::string> result;
        std::string remaining = msg;
        
        while (!remaining.empty()) {
            if (static_cast<int>(remaining.length()) <= max_width) {
                result.push_back(remaining);
                break;
            }
            
            // ✅ 尝试在空格处断开
            size_t cut_pos = remaining.find_last_of(' ', max_width);
            if (cut_pos == std::string::npos || cut_pos == 0) {
                cut_pos = max_width;
            } else {
                // 保持单词完整，但不要切掉空格
                cut_pos = cut_pos + 1;
            }
            
            result.push_back(remaining.substr(0, cut_pos));
            remaining = remaining.substr(cut_pos);
            
            // 去除行首空格
            if (!remaining.empty() && remaining[0] == ' ') {
                remaining = remaining.substr(1);
            }
        }
        
        return result;
    }

    // 完整重绘
    void redrawAll() {
        if (!msg_win_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        
        werase(msg_win_);
        box(msg_win_, 0, 0);

        int max_y = getmaxy(msg_win_) - 1;
        int max_x = getmaxx(msg_win_) - 4;
        if(max_x < 10) {
            max_x = 10;
        }
        
        int display_lines = max_y - 2;
        if (display_lines <= 0) return;
        
        size_t start = 0;
        if (lines_.size() > static_cast<size_t>(display_lines)) {
            start = lines_.size() - display_lines;
        }

        int y = 1;
        for (size_t i = start; i < lines_.size() && y < max_y; ++i) {
            std::string line = lines_[i];
            if (static_cast<int>(line.length()) > max_x) {
                line = line.substr(0, max_x - 3) + "...";
            }
            mvwprintw(msg_win_, y, 2, "%-*s", max_x, " ");
            mvwprintw(msg_win_, y++, 2, "%s", line.c_str());
        }
        wrefresh(msg_win_);
    }
};