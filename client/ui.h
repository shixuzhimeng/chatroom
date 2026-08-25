#pragma once

#include <ncursesw/curses.h>

#include <algorithm>
#include <codecvt>
#include <cwchar>
#include <deque>
#include <functional>
#include <locale>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

class cUI {
public:
    cUI(): msg_win_(nullptr),
          input_win_(nullptr),
          status_win_(nullptr),
          running_(false),
          msg_scroll_(0),
          input_cursor_(0) {
    }

    ~cUI() {
        if (msg_win_)
            delwin(msg_win_);

        if (input_win_)
            delwin(input_win_);

        if (status_win_)
            delwin(status_win_);

        // 关闭终端括号粘贴模式
        const char* disable_bp = "\x1b[?2004l";
        write(STDOUT_FILENO, disable_bp, 8);

        endwin();
    }

    bool UIinit() {
        setlocale(LC_ALL, "");

        if (initscr() == nullptr)
            return false;

        cbreak();
        noecho();
        keypad(stdscr, TRUE);

        curs_set(1);

        // 启用终端括号粘贴模式，用于正确识别粘贴内容
        const char* enable_bp = "\x1b[?2004h";
        write(STDOUT_FILENO, enable_bp, 8);

        int h, w;
        getmaxyx(stdscr, h, w);

        if (h < 8 || w < 20) {
            endwin();
            return false;
        }

        const int status_h = 1;
        const int input_h = 5;
        const int msg_h = h - status_h - input_h;

        status_win_ = newwin(status_h, w, 0, 0);
        msg_win_ = newwin(msg_h, w, status_h, 0);
        input_win_ = newwin(
            input_h,
            w,
            status_h + msg_h,
            0
        );

        if (!status_win_ || !msg_win_ || !input_win_)
            return false;

        keypad(input_win_, TRUE);

        scrollok(msg_win_, FALSE);

        drawStatus();
        redrawMessages();
        redrawInput();

        refresh();

        running_ = true;

        return true;
    }

    void setStatus(const std::string& text) {
        if (!status_win_)
            return;

        std::lock_guard<std::mutex> lock(ui_mutex_);

        werase(status_win_);

        int w = getmaxx(status_win_);

        mvwprintw(
            status_win_,
            0,
            1,
            "%.*s",
            std::max(0, w - 2),
            text.c_str()
        );

        wrefresh(status_win_);
    }

    void displayMessage(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(message_mutex_);

            messages_.push_back(msg);

            if (messages_.size() > MAX_MESSAGES) {
                messages_.pop_front();
            }

            msg_scroll_ = 0;
        }

        redrawMessages();
        redrawInput();
    }

    void displaySystem(const std::string& msg) {
        displayMessage("[系统] " + msg);
    }

    void displayError(const std::string& msg) {
        displayMessage("[错误] " + msg);
    }

    std::string getInput(std::function<void(const std::string&)> sendLine = nullptr, std::function<void()> processMessages = nullptr) {
        if (!input_win_)
            return "";

        // 用带超时的阻塞读既能及时处理到达的消息，又不会在粘贴时丢字符
        wtimeout(input_win_, 50);

        std::wstring input;
        size_t cursor = 0;
        bool pasting = false;
        bool submit = false;

        // 粘贴态下：把当前已累积的一行立即发送出去，然后清空，实现"边粘贴边逐行发送"
        auto flushLine = [&]() {
            if (input.empty()) {
                return;
            }
            if (sendLine) {
                sendLine(wideToUtf8(input));
            }
            input.clear();
            cursor = 0;
        };

        while(running_ && !submit) {
            if(processMessages) {
                processMessages();
            }

            redrawInput(input, cursor);

            wint_t ch;
            int ret = wget_wch(input_win_, &ch);

            if(ret == ERR) {
                continue;
            }

            if(ret == KEY_CODE_YES) {
                switch (ch) {
                case KEY_ENTER:
                    if(pasting) {
                        flushLine();
                    } else {
                        submit = true;
                    }
                    break;

                case KEY_BACKSPACE:
                    if (cursor > 0) {
                        input.erase(cursor - 1, 1);
                        --cursor;
                    }
                    break;

                case KEY_LEFT:
                    if (cursor > 0)
                        --cursor;
                    break;
                case KEY_RIGHT:
                    if (cursor < input.size())
                        ++cursor;
                    break;
                case KEY_HOME:
                    cursor = 0;
                    break;
                case KEY_END:
                    cursor = input.size();
                    break;
                // 消息窗口滚动
                case KEY_PPAGE:
                    scrollMessages(10);
                    break;
                case KEY_NPAGE:
                    scrollMessages(-10);
                    break;
                case KEY_UP:
                    scrollMessages(1);
                    break;
                case KEY_DOWN:
                    scrollMessages(-1);
                    break;

                default:
                    break;
                }
                continue;
            }

            if(ret == OK) {
                // Ctrl+Y：复制最后一条消息到剪贴板
                if(ch == 0x19) {
                    copyLastMessage();
                    continue;
                }

                // ESC：可能是括号粘贴的开始/结束标记
                if(ch == 0x1b) {
                    int paste_evt = tryDetectPaste();
                    if(paste_evt == 1) {
                        pasting = true;
                    } else if(paste_evt == 2) {
                        pasting = false;
                        flushLine();   // 发送最后一行
                        submit = true; // 粘贴结束，退出输入循环
                    }
                    continue;
                }

                // 回车提交（粘贴态下按行立即发送）
                if(ch == L'\n' || ch == L'\r') {
                    if(pasting) {
                        flushLine();
                    } else {
                        submit = true;
                    }
                    continue;
                }

                if(ch == 127 || ch == 8) {
                    if(cursor > 0) {
                        input.erase(cursor - 1, 1);
                        --cursor;
                    }
                    continue;
                }

                // 可打印字符
                if(iswprint(ch)) {
                    input.insert(
                        input.begin() + cursor,
                        static_cast<wchar_t>(ch)
                    );
                    ++cursor;
                }

                continue;
            }
        }

        wtimeout(input_win_, -1);
        redrawInput();
        return wideToUtf8(input);
    }

    // 消息窗口滚动
    void scrollMessages(int delta) {
        int content_height = 0;
        int content_width = 0;
        int total_lines = 0;
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            int height = getmaxy(msg_win_);
            int width = getmaxx(msg_win_);
            content_height = height - 2;
            content_width = width - 4;
            if(content_height > 0 && content_width > 0) {
                for(const auto& msg : messages_) {
                    auto wmsg = utf8ToWide(msg);
                    auto wl = wrapText(wmsg, content_width);
                    total_lines += static_cast<int>(wl.size());
                }
            }
        }

        int max_scroll = total_lines - content_height;
        if(max_scroll < 0)
            max_scroll = 0;

        int next = msg_scroll_ + delta;
        if(next < 0)
            next = 0;
        if(next > max_scroll)
            next = max_scroll;

        if(next != msg_scroll_) {
            msg_scroll_ = next;
            redrawMessages();
        }
    }

    // 在读到ESC后调用，尝试识别括号粘贴转义序列
    // 返回 1 = 开始粘贴 "[200~"，2 = 结束粘贴 "[201~"，0 = 其他
    int tryDetectPaste() {
        nodelay(input_win_, TRUE);
        wint_t seq[5];
        int rs[5];
        for(int i = 0; i < 5; ++i) {
            rs[i] = wget_wch(input_win_, &seq[i]);
        }
        nodelay(input_win_, FALSE);

        if(rs[0] == OK && seq[0] == L'[' && rs[1] == OK && seq[1] == L'2' &&
           rs[2] == OK && seq[2] == L'0' && rs[3] == OK && seq[3] == L'0' &&
           rs[4] == OK && seq[4] == L'~') {
            return 1;
        }
        if(rs[0] == OK && seq[0] == L'[' && rs[1] == OK && seq[1] == L'2' &&
           rs[2] == OK && seq[2] == L'0' && rs[3] == OK && seq[3] == L'1' &&
           rs[4] == OK && seq[4] == L'~') {
            return 2;
        }
        return 0;
    }

    // 复制最后一条消息到系统剪贴板（OSC 52）
    void copyLastMessage() {
        std::string last;
        {
            std::lock_guard<std::mutex> lock(message_mutex_);
            if(!messages_.empty()) {
                last = messages_.back();
            }
        }
        if(last.empty())
            return;

        std::string b64 = base64Encode(last);
        std::string osc = "\x1b]52;c;" + b64 + "\x07";
        if(write(STDOUT_FILENO, osc.data(), osc.size()) < 0) {
            // 忽略写入失败
        }
        //displaySystem("已复制最后一条消息到剪贴板");
    }

    static std::string base64Encode(const std::string& data) {
        static const char* tbl =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0;
        int bits = -6;
        for(unsigned char c : data) {
            val = (val << 8) + c;
            bits += 8;
            while(bits >= 0) {
                out.push_back(tbl[(val >> bits) & 0x3F]);
                bits -= 6;
            }
        }
        if(bits > -6) {
            out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
        }
        while(out.size() % 4) {
            out.push_back('=');
        }
        return out;
    }

    void run(std::function<void(const std::string&)> handler, std::function<void()> processMessages) {
        if(!handler)
            return;

        running_ = true;

        while(running_) {
            if(processMessages) {
                processMessages();
            }

            std::string input = getInput(handler, processMessages);
            if(input.empty()) {
                continue;
            }
            if(input == "/quit" || input == "exit") {
                running_ = false;
                break;
            }
            if(handler) {
                handler(input);
            }
        }
    }

    void stop() {
        running_ = false;
    }

    void refresh() {
        if (!status_win_ || !msg_win_ || !input_win_)
            return;

        redrawStatus();
        redrawMessages();
        redrawInput();
    }

    void clearScreen() {
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        messages_.clear();
        msg_scroll_ = 0;
    }
        redrawMessages();
    }

private:
    WINDOW* msg_win_;
    WINDOW* input_win_;
    WINDOW* status_win_;
    std::deque<std::string> messages_;

    mutable std::mutex message_mutex_;
    mutable std::mutex ui_mutex_;

    bool running_;

    int msg_scroll_;
    size_t input_cursor_;
    static constexpr size_t MAX_MESSAGES = 1000;

    std::wstring utf8ToWide(const std::string& str) {
        std::wstring result;
        mbstate_t state{};
        const char* src = str.data();
        size_t len = str.size();

        while(len > 0) {
            wchar_t wc;
            size_t ret = mbrtowc(&wc, src, len, &state);

            if(ret == static_cast<size_t>(-1)) {
                // 非法 UTF-8
                result.push_back(L'?');
                ++src;
                --len;
                state = mbstate_t{};
                continue;
            }
            if(ret == static_cast<size_t>(-2)) {
                break;
            }

            if(ret == 0) {
                break;
            }

            result.push_back(wc);

            src += ret;
            len -= ret;
        }

        return result;
    }

    std::string wideToUtf8(const std::wstring& str) {
        std::string result;
        mbstate_t state{};
        char buffer[MB_LEN_MAX];

        for(wchar_t wc : str) {
            size_t ret = wcrtomb(buffer, wc, &state);

            if(ret == static_cast<size_t>(-1)) {
                result += '?';
                state = mbstate_t{};
                continue;
            }

            result.append(buffer, ret);
        }

        return result;
    }

    int charWidth(wchar_t wc) {
        int width = wcwidth(wc);

        if(width < 0)
            return 0;

        return width;
    }

    std::vector<std::wstring> wrapText(const std::wstring& text, int max_width) {
        std::vector<std::wstring> result;
        if (max_width <= 0)
            return result;

        std::wstring current;
        int current_width = 0;

        for(wchar_t wc : text) {
            if(wc == L'\n') {
                result.push_back(current);

                current.clear();
                current_width = 0;

                continue;
            }

            int width = charWidth(wc);

            if(current_width + width > max_width) {

                result.push_back(current);

                current.clear();
                current_width = 0;
            }

            current.push_back(wc);
            current_width += width;
        }

        if(!current.empty() || result.empty()) {
            result.push_back(current);
        }

        return result;
    }


    std::vector<std::wstring> buildDisplayLines(int width) {
        std::vector<std::wstring> result;
        std::lock_guard<std::mutex> lock(message_mutex_);
        for(const auto& msg : messages_) {
            std::wstring wmsg = utf8ToWide(msg);
            auto lines = wrapText(wmsg, width);
            result.insert(result.end(), lines.begin(), lines.end());
        }
        return result;
    }

    void drawStatus() {
        if(!status_win_)
            return;
        werase(status_win_);
        int width = getmaxx(status_win_);
        mvwprintw(
            status_win_,
            0,
            1,
            "%.*s",
            std::max(0, width - 2),
            "Status: Connected"
        );

        wrefresh(status_win_);
    }

    void redrawStatus() {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        drawStatus();
    }

    void redrawMessages() {
        if(!msg_win_)
            return;

        std::lock_guard<std::mutex> ui_lock(ui_mutex_);
        int height = getmaxy(msg_win_);
        int width = getmaxx(msg_win_);
        int content_width = width - 4;
        int content_height = height - 2;

        if (content_width <= 0 ||
            content_height <= 0)
            return;

        auto display_lines = buildDisplayLines(content_width);

        werase(msg_win_);
        box(msg_win_, 0, 0);

        if(display_lines.empty()) {
            wrefresh(msg_win_);
            return;
        }

        int last = static_cast<int>(display_lines.size()) - 1;
        int end_line = last - msg_scroll_;
        if(end_line < 0)
            end_line = 0;
        int start_line = end_line - content_height + 1;

        if(start_line < 0)
            start_line = 0;

        for(int i = start_line; i <= end_line && i < static_cast<int>(display_lines.size()); ++i) {
            int y = 1 + (i - start_line);
            mvwaddwstr(msg_win_, y, 2, display_lines[i].c_str());
        }

        wrefresh(msg_win_);
    }

    void redrawInput(const std::wstring& input, size_t cursor) {
        if (!input_win_)
            return;

        std::lock_guard<std::mutex> lock(ui_mutex_);
        werase(input_win_);
        box(input_win_, 0, 0);
        int height = getmaxy(input_win_);
        int width = getmaxx(input_win_);
        int content_width = width - 4;
        if(content_width <= 0)
            return;

        std::vector<std::wstring> lines;
        std::wstring current;

        int current_width = 0;
        int cursor_line = 0;
        int cursor_column = 0;
        size_t index = 0;

        for(wchar_t wc : input) {
            if(wc == L'\n') {
                lines.push_back(current);
                current.clear();
                current_width = 0;
                ++index;

                if(index <= cursor) {
                    ++cursor_line;
                    cursor_column = 0;
                }
                continue;
            }

            int width_wc = charWidth(wc);
            if(current_width + width_wc > content_width) {
                lines.push_back(current);
                current.clear();
                current_width = 0;
                ++cursor_line;
                cursor_column = 0;
            }

            current.push_back(wc);
            current_width += width_wc;

            if (index < cursor) {
                cursor_column += width_wc;
            }

            ++index;
        }

        lines.push_back(current);
        cursor_line = 0;
        cursor_column = 0;
        int line_width = 0;

        for(size_t i = 0; i < cursor; ++i) {
            wchar_t wc = input[i];
            if(wc == L'\n') {
                ++cursor_line;
                line_width = 0;
                continue;
            }

            int cw = charWidth(wc);
            if(line_width + cw > content_width) {
                ++cursor_line;
                line_width = 0;
            }

            line_width += cw;
        }

        cursor_column = line_width;

        int visible_height = height - 2;
        int first_line = 0;

        if(cursor_line >= visible_height) {
            first_line = cursor_line - visible_height + 1;
        }
        for(int i = first_line; i < static_cast<int>(lines.size()) && i < first_line + visible_height; ++i) {
            mvwaddwstr(input_win_, 1 + i - first_line, 2, lines[i].c_str());
        }

        int screen_cursor_y = 1 + cursor_line - first_line;
        int screen_cursor_x = 2 + cursor_column;

        screen_cursor_y = std::clamp(screen_cursor_y, 1, height - 2);
        screen_cursor_x = std::clamp(screen_cursor_x, 2, width - 2);

        wmove(input_win_, screen_cursor_y, screen_cursor_x);

        wrefresh(input_win_);
    }

    void redrawInput() {
        redrawInput(L"", 0);
    }
};