#include "chat_client.h"
#include "ui.h"
#include "tool/logging.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <getopt.h>
#include <deque>
#include <cstring>
#include <csignal>
#include <filesystem>

static std::unique_ptr<ChatClient> client;
static std::unique_ptr<cUI> ui;
static std::atomic<bool> running(true);
static std::deque<std::string> pending_messages;
static std::mutex pending_mutex;

enum class MenuState {
    MAIN,              // 主菜单
    LOGIN,             // 登录
    REGISTER,          // 注册
    DELETE_ACCOUNT,    // 删除帐号
    CHAT,              // 聊天
    SEND_FILE,         // 发送文件
    SEND_GROUP_FILE,   // 发送群组文件 
    DOWNLOAD_FILE,     // 下载文件
    CREATE_GROUP,      // 创建群组
    JOIN_GROUP,        // 加入群组
    LEAVE_GROUP,       // 退出群组
    DISMISS_GROUP,     // 解散群组
    SET_ADMIN,         // 设置管理员
    KICK_MEMBER,       // 踢人
    PENDING_REQUESTS,  // 查看待审批申请
    APPROVE_REQUEST,   // 审批入群申请
    HISTORY,           // 历史记录
    GROUP_HISTORY,     // 群聊历史
    RECALL,            // 撤回消息
    GROUP_RECALL,      // 撤回群组消息
    FRIEND_ADD,        // 添加好友
    FRIEND_DELETE,     // 删除好友
    FRIEND_PROCESS,    // 好友申请
    BLOCK_USER,        // 屏蔽好友
    MARK_READ,         // 标记私聊已读
    MARK_GROUP_READ    // 标记群组已读
};

static MenuState current_state = MenuState::MAIN;
static std::string current_input_buffer;

static std::vector<std::string> CommandParser(const std::string& cmd) {
    std::vector<std::string> parts;
    std::stringstream ss(cmd);
    std::string part;
    while (ss >> part) {
        parts.push_back(part);
    }
    return parts;
}

void showMainMenu() {
    ui->clearScreen();
    ui->displaySystem("                         ChatRoom                        ");
    if (client && client->isLoggedIn()) {
        ui->displaySystem("已登录： UID " + std::to_string(client->getUserId()));
    } else {
        ui->displaySystem("未登录                                                ");
    }
    ui->displaySystem("                                                        ");
    ui->displaySystem("账号管理");
    ui->displaySystem("1.登录");
    ui->displaySystem("2.注册");
    ui->displaySystem("3.删除帐号");
    ui->displaySystem("4.登出");
    ui->displaySystem("                                                         ");
    ui->displaySystem("好友管理");
    ui->displaySystem("5.好友列表");
    ui->displaySystem("6.添加好友");
    ui->displaySystem("7.删除好友");
    ui->displaySystem("8.处理好友请求");
    ui->displaySystem("                                                         ");
    ui->displaySystem("群组管理");
    ui->displaySystem("9.群组列表");
    ui->displaySystem("10.群成员列表");
    ui->displaySystem("11.创建群组");
    ui->displaySystem("12.加入群组");
    ui->displaySystem("13.退出群组");
    ui->displaySystem("14.解散群组");
    ui->displaySystem("15.设置管理员");
    ui->displaySystem("16.踢人");
    ui->displaySystem("17.待处理的入群申请");
    ui->displaySystem("18.处理入群申请");
    ui->displaySystem("                                                        ");
    ui->displaySystem("消息管理");
    ui->displaySystem("19.开始聊天");
    ui->displaySystem("20.历史消息");
    ui->displaySystem("21.群组历史消息");
    ui->displaySystem("22.撤回消息");
    ui->displaySystem("23.撤回群组消息");
    ui->displaySystem("24.标记为已读");
    ui->displaySystem("25.标记群组消息为已读");
    ui->displaySystem("                                                        ");
    ui->displaySystem("文件管理");
    ui->displaySystem("26.发送文件");
    ui->displaySystem("27.发送群组文件");
    ui->displaySystem("28.下载文件");
    ui->displaySystem("29.离线文件列表");
    ui->displaySystem("                                                        ");
    ui->displaySystem("屏蔽管理");
    ui->displaySystem("30.屏蔽好友");
    ui->displaySystem("31.取消屏蔽");
    ui->displaySystem("32.屏蔽列表");
    ui->displaySystem("                                                        ");
    ui->displaySystem("33 清屏");
    ui->displaySystem("0.退出");
    ui->displaySystem("请选择：");
}

void showLoginMenu() {
    ui->displaySystem("登录");
    ui->displaySystem("请输入用户名: ");
}

void showRegisterMenu() {
    ui->displaySystem("注册新账号");
    ui->displaySystem("请输入用户名: ");
}

void showDeleteAccountMenu() {
    ui->displaySystem("删除账号");
    ui->displaySystem("警告：此操作不可恢复！");
    ui->displaySystem("请输入密码确认: ");
}

void showFriendAddMenu() {
    ui->displaySystem("添加好友");
    ui->displaySystem("请输入好友ID: ");
}

void showFriendDeleteMenu() {
    ui->displaySystem("删除好友");
    ui->displaySystem("请输入要删除的好友ID: ");
}

void showFriendProcessMenu() {
    ui->displaySystem("处理好友请求");
    ui->displaySystem("请输入请求ID 和 接受/拒绝 (如: 12345 true): ");
}

void showGroupMembersMenu() {
    ui->displaySystem("群成员列表");
    ui->displaySystem("请输入群组ID: ");
}

void showJoinGroupMenu() {
    ui->displaySystem("加入群组");
    ui->displaySystem("请输入群组ID [附言消息]: ");
}

void showLeaveGroupMenu() {
    ui->displaySystem("退出群组");
    ui->displaySystem("请输入群组ID: ");
}

void showDismissGroupMenu() {
    ui->displaySystem("解散群组");
    ui->displaySystem("请输入群组ID: ");
}

void showSetAdminMenu() {
    ui->displaySystem("设置管理员");
    ui->displaySystem("请输入: 群组ID 用户ID true/false (如: 12345 67890 true): ");
}

void showKickMemberMenu() {
    ui->displaySystem("踢出成员");
    ui->displaySystem("请输入: 群组ID 用户ID: ");
}

void showPendingRequestsMenu() {
    ui->displaySystem("待处理申请");
    ui->displaySystem("请输入群组ID: ");
}

void showApproveRequestMenu() {
    ui->displaySystem("处理入群申请");
    ui->displaySystem("请输入: 请求ID true/false: ");
}

void showHistoryMenu() {
    ui->displaySystem("历史消息");
    ui->displaySystem("请输入用户ID [数量，默认50]: ");
}

void showGroupHistoryMenu() {
    ui->displaySystem("群组历史消息");
    ui->displaySystem("请输入群组ID [数量，默认50]: ");
}

void showRecallMenu() {
    ui->displaySystem("撤回消息");
    ui->displaySystem("请输入消息ID: ");
}

void showGroupRecallMenu() {
    ui->displaySystem("撤回群消息");
    ui->displaySystem("请输入消息ID: ");
}

void showChatMenu() {
    ui->displaySystem("开始聊天");
    ui->displaySystem("请输入好友ID或群组ID (G+群组ID 如 G12345): ");
}

void showSendFileMenu() {
    ui->displaySystem("发送文件");
    ui->displaySystem("请输入目标用户ID: ");
}

void showSendGroupFileMenu() {
    ui->displaySystem("发送群文件");
    ui->displaySystem("请输入群组ID: ");
}

void showDownloadMenu() {
    ui->displaySystem("下载文件");
    ui->displaySystem("请输入文件ID: ");
}

void showBlockMenu() {
    ui->displaySystem("屏蔽用户");
    ui->displaySystem("请输入要屏蔽的用户ID: ");
}

void showUnblockMenu() {
    ui->displaySystem("取消屏蔽");
    ui->displaySystem("请输入要取消屏蔽的用户ID: ");
}

void showCreateGroupMenu() {
    ui->displaySystem("创建群组");
    ui->displaySystem("请输入: 群名称 描述 [加群方式0直接/1需申请/2禁止] [public/private] [人数上限]");
    ui->displaySystem("示例: 技术群 讨论技术 1 public 100");
}

void showMarkReadMenu() {
    ui->displaySystem("标记已读");
    ui->displaySystem("请输入用户ID: ");
}

void showMarkGroupReadMenu() {
    ui->displaySystem("标记群组已读");
    ui->displaySystem("请输入群组ID: ");
}

void processMainMenu(const std::string& input) {
    if (input.empty()) return;
    
    int choice = -1;
    try {
        choice = std::stoi(input);
    } catch (...) {
        ui->displayError("请输入数字");
        return;
    }
    
    // 检查是否需要登录
    bool need_login = (choice >= 5 && choice <= 32 && choice != 11);
    if (need_login && !(client && client->isLoggedIn())) {
        ui->displayError("请先登录");
        return;
    }
    
    switch (choice) {
        case 0:
            if (client) client->disconnect();
            running = false;
            break;
            
        case 1:
            current_state = MenuState::LOGIN;
            current_input_buffer.clear();
            showLoginMenu();
            break;
        case 2:
            current_state = MenuState::REGISTER;
            current_input_buffer.clear();
            showRegisterMenu();
            break;
        case 3:
            if (!(client && client->isLoggedIn())) {
                ui->displayError("请先登录");
                return;
            }
            current_state = MenuState::DELETE_ACCOUNT;
            current_input_buffer.clear();
            showDeleteAccountMenu();
            break;
        case 4:
            if (client && client->isLoggedIn()) {
                client->Logout();
                ui->displaySystem("已登出");
            } else {
                ui->displayError("未登录");
            }
            break;
            
        case 5:
            client->getFriendList();
            break;
        case 6:
            current_state = MenuState::FRIEND_ADD;
            showFriendAddMenu();
            break;
        case 7:
            current_state = MenuState::FRIEND_DELETE;
            showFriendDeleteMenu();
            break;
        case 8:
            current_state = MenuState::FRIEND_PROCESS;
            showFriendProcessMenu();
            break;
            
        case 9:
            client->getGroupList();
            break;
        case 10:
            current_state = MenuState::GROUP_HISTORY;
            showGroupMembersMenu();
            break;
        case 11:
            current_state = MenuState::CREATE_GROUP;
            current_input_buffer.clear();
            showCreateGroupMenu();
            break;
        case 12:
            current_state = MenuState::JOIN_GROUP;
            current_input_buffer.clear();
            showJoinGroupMenu();
            break;
        case 13:
            current_state = MenuState::LEAVE_GROUP;
            showLeaveGroupMenu();
            break;
        case 14:
            current_state = MenuState::DISMISS_GROUP;
            showDismissGroupMenu();
            break;
        case 15:
            current_state = MenuState::SET_ADMIN;
            showSetAdminMenu();
            break;
        case 16:
            current_state = MenuState::KICK_MEMBER;
            showKickMemberMenu();
            break;
        case 17:
            current_state = MenuState::PENDING_REQUESTS;
            showPendingRequestsMenu();
            break;
        case 18:
            current_state = MenuState::APPROVE_REQUEST;
            showApproveRequestMenu();
            break;
            
        case 19:
            current_state = MenuState::CHAT;
            current_input_buffer.clear();
            showChatMenu();
            break;
        case 20:
            current_state = MenuState::HISTORY;
            showHistoryMenu();
            break;
        case 21:
            current_state = MenuState::GROUP_HISTORY;
            showGroupHistoryMenu();
            break;
        case 22:
            current_state = MenuState::RECALL;
            showRecallMenu();
            break;
        case 23:
            current_state = MenuState::GROUP_RECALL;
            showGroupRecallMenu();
            break;
        case 24:
            current_state = MenuState::MARK_READ;
            showMarkReadMenu();
            break;
        case 25:
            current_state = MenuState::MARK_GROUP_READ;
            showMarkGroupReadMenu();
            break;
            
        case 26:
            current_state = MenuState::SEND_FILE;
            current_input_buffer.clear();
            showSendFileMenu();
            break;
        case 27:
            current_state = MenuState::SEND_GROUP_FILE;
            current_input_buffer.clear();
            showSendGroupFileMenu();
            break;
        case 28:
            current_state = MenuState::DOWNLOAD_FILE;
            showDownloadMenu();
            break;
        case 29:
            client->getOfflineFiles();
            break;
            
        // === 屏蔽管理 ===
        case 30:
            current_state = MenuState::BLOCK_USER;
            showBlockMenu();
            break;
        case 31:
            current_state = MenuState::FRIEND_DELETE;
            showUnblockMenu();
            break;
        case 32:
            client->getBlockList();
            break;
            
        case 33:
            ui->clearScreen();
            showMainMenu();
            break;
            
        default:
            ui->displayError("无效选择，请输入 0-33");
    }
}

void processLogin(const std::string& input) {
    if (input.empty()) { showLoginMenu(); return; }
    if (current_input_buffer.empty()) {
        current_input_buffer = input;
        ui->displaySystem("请输入密码: ");
    } else {
        client->Login(current_input_buffer, input, "client_" + tool::randString(8));
        current_input_buffer.clear();
        current_state = MenuState::MAIN;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        showMainMenu();
    }
}

void processRegister(const std::string& input) {
    if (input.empty()) { showRegisterMenu(); return; }
    if (current_input_buffer.empty()) {
        current_input_buffer = input;
        ui->displaySystem("请输入密码: ");
    } else {
        std::string username = current_input_buffer;
        std::string password = input;
        std::string email = username + "@qq.com";
        client->registerUser(username, password, email, username);
        current_input_buffer.clear();
        current_state = MenuState::MAIN;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        showMainMenu();
    }
}

void processDeleteAccount(const std::string& input) {
    if (input.empty()) { showDeleteAccountMenu(); return; }
    ui->displaySystem("正在删除账号...");
    client->deleteUser(input);
    current_input_buffer.clear();
    current_state = MenuState::MAIN;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    showMainMenu();
}

void processChat(const std::string& input) {
    if (input.empty()) { showChatMenu(); return; }
    
    if (input[0] == 'G' || input[0] == 'g') {
        try {
            uint64_t gid = std::stoull(input.substr(1));
            client->setCurMod(gid, true);
            if (ui) ui->setStatus("聊天中：群组 " + std::to_string(gid));
            ui->displaySystem("已进入群组 " + std::to_string(gid) + " 的聊天模式");
            ui->displaySystem("输入消息直接发送，输入 /back 退出聊天");
        } catch (...) {
            ui->displayError("无效的群组ID");
        }
    } else {
        try {
            uint64_t uid = std::stoull(input);
            client->setCurMod(uid, false);
            if (ui) ui->setStatus("聊天中：用户 " + std::to_string(uid));
            ui->displaySystem("已进入与用户 " + std::to_string(uid) + " 的聊天模式");
            ui->displaySystem("输入消息直接发送，输入 /back 退出聊天");
        } catch (...) {
            ui->displayError("无效的用户ID");
        }
    }
}

void processSendFile(const std::string& input) {
    if (input.empty()) { showSendFileMenu(); return; }
    if (current_input_buffer.empty()) {
        try {
            std::stoull(input);
            current_input_buffer = input;
            ui->displaySystem("请输入文件路径: ");
        } catch (...) {
            ui->displayError("无效的用户ID");
        }
    } else {
        try {
            uint64_t to_uid = std::stoull(current_input_buffer);
            ui->displaySystem("正在发送文件...");
            client->sendFile(to_uid, input, "");
            current_input_buffer.clear();
            current_state = MenuState::MAIN;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            showMainMenu();
        } catch (...) {
            ui->displayError("发送失败");
        }
    }
}

void processSendGroupFile(const std::string& input) {
    if (input.empty()) { showSendGroupFileMenu(); return; }
    if (current_input_buffer.empty()) {
        try {
            std::stoull(input);
            current_input_buffer = input;
            ui->displaySystem("请输入文件路径: ");
        } catch (...) {
            ui->displayError("无效的群组ID");
        }
    } else {
        try {
            uint64_t group_id = std::stoull(current_input_buffer);
            ui->displaySystem("正在发送文件到群组...");
            client->sendGroupFile(group_id, input, "");
            current_input_buffer.clear();
            current_state = MenuState::MAIN;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            showMainMenu();
        } catch (...) {
            ui->displayError("发送失败");
        }
    }
}

void processDownloadFile(const std::string& input) {
    if (input.empty()) { showDownloadMenu(); return; }
    ui->displaySystem("正在下载文件: " + input);
    client->downloadFile(input);
    current_state = MenuState::MAIN;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    showMainMenu();
}

void processFriendAdd(const std::string& input) {
    if (input.empty()) { showFriendAddMenu(); return; }
    try {
        uint64_t uid = std::stoull(input);
        client->sendFriendRequest(uid, "加个好友吧！");
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的用户ID");
    }
}

void processFriendDelete(const std::string& input) {
    if (input.empty()) { showFriendDeleteMenu(); return; }
    try {
        client->deleteFriend(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的用户ID");
    }
}

void processFriendProcess(const std::string& input) {
    if (input.empty()) { showFriendProcessMenu(); return; }
    auto parts = CommandParser(input);
    if (parts.size() >= 2) {
        try {
            client->processFriendRequest(std::stoull(parts[0]), parts[1] == "true");
            current_state = MenuState::MAIN;
            showMainMenu();
        } catch (...) {
            ui->displayError("格式错误，请输入: 请求ID true/false");
        }
    } else {
        ui->displayError("格式错误，请输入: 请求ID true/false");
    }
}

void processGroupMembers(const std::string& input) {
    if (input.empty()) { showGroupMembersMenu(); return; }
    try {
        client->getGroupMembers(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processJoinGroup(const std::string& input) {
    if (input.empty()) { showJoinGroupMenu(); return; }
    auto parts = CommandParser(input);
    try {
        uint64_t gid = std::stoull(parts[0]);
        std::string msg = parts.size() > 1 ? parts[1] : "我想加入群组";
        client->joinGroup(gid, msg);
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processLeaveGroup(const std::string& input) {
    if (input.empty()) { showLeaveGroupMenu(); return; }
    try {
        client->leaveGroup(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processDismissGroup(const std::string& input) {
    if (input.empty()) { showDismissGroupMenu(); return; }
    try {
        client->dismissGroup(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processSetAdmin(const std::string& input) {
    if (input.empty()) { showSetAdminMenu(); return; }
    auto parts = CommandParser(input);
    if (parts.size() >= 3) {
        try {
            client->setAdmin(std::stoull(parts[0]), std::stoull(parts[1]), parts[2] == "true");
            current_state = MenuState::MAIN;
            showMainMenu();
        } catch (...) {
            ui->displayError("格式错误，请输入: 群组ID 用户ID true/false");
        }
    } else {
        ui->displayError("格式错误，请输入: 群组ID 用户ID true/false");
    }
}

void processKickMember(const std::string& input) {
    if (input.empty()) { showKickMemberMenu(); return; }
    auto parts = CommandParser(input);
    if (parts.size() >= 2) {
        try {
            client->kickMember(std::stoull(parts[0]), std::stoull(parts[1]));
            current_state = MenuState::MAIN;
            showMainMenu();
        } catch (...) {
            ui->displayError("格式错误，请输入: 群组ID 用户ID");
        }
    } else {
        ui->displayError("格式错误，请输入: 群组ID 用户ID");
    }
}

void processPendingRequests(const std::string& input) {
    if (input.empty()) { showPendingRequestsMenu(); return; }
    try {
        client->getPendingRequests(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processApproveRequest(const std::string& input) {
    if (input.empty()) { showApproveRequestMenu(); return; }
    auto parts = CommandParser(input);
    if (parts.size() >= 2) {
        try {
            client->processJoinRequest(std::stoull(parts[0]), parts[1] == "true");
            current_state = MenuState::MAIN;
            showMainMenu();
        } catch (...) {
            ui->displayError("格式错误，请输入: 请求ID true/false");
        }
    } else {
        ui->displayError("格式错误，请输入: 请求ID true/false");
    }
}

void processHistory(const std::string& input) {
    if (input.empty()) { showHistoryMenu(); return; }
    auto parts = CommandParser(input);
    try {
        uint64_t uid = std::stoull(parts[0]);
        int limit = parts.size() > 1 ? std::stoi(parts[1]) : 50;
        client->getHistory(uid, limit);
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("格式错误，请输入: 用户ID [数量]");
    }
}

void processGroupHistory(const std::string& input) {
    if (input.empty()) { showGroupHistoryMenu(); return; }
    auto parts = CommandParser(input);
    try {
        uint64_t gid = std::stoull(parts[0]);
        int limit = parts.size() > 1 ? std::stoi(parts[1]) : 50;
        client->getGroupHistory(gid, limit);
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("格式错误，请输入: 群组ID [数量]");
    }
}

void processRecall(const std::string& input) {
    if (input.empty()) { showRecallMenu(); return; }
    try {
        client->recallMessage(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的消息ID");
    }
}

void processGroupRecall(const std::string& input) {
    if (input.empty()) { showGroupRecallMenu(); return; }
    try {
        client->recallGroupMessage(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的消息ID");
    }
}

void processBlock(const std::string& input) {
    if (input.empty()) { showBlockMenu(); return; }
    try {
        client->blockUser(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的用户ID");
    }
}

void processUnblock(const std::string& input) {
    if (input.empty()) { showUnblockMenu(); return; }
    try {
        client->unblockUser(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的用户ID");
    }
}

void processMarkRead(const std::string& input) {
    if (input.empty()) {
        client->markRead(0);  // 不输入则全部已读
        current_state = MenuState::MAIN;
        showMainMenu();
        return;
    }
    try {
        client->markRead(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的用户ID");
    }
}

void processMarkGroupRead(const std::string& input) {
    if (input.empty()) { showMarkGroupReadMenu(); return; }
    try {
        client->markGroupRead(std::stoull(input));
        current_state = MenuState::MAIN;
        showMainMenu();
    } catch (...) {
        ui->displayError("无效的群组ID");
    }
}

void processCreateGroup(const std::string& input) {
    if (input.empty()) { showCreateGroupMenu(); return; }
    
    if (current_input_buffer.empty()) {
        auto parts = CommandParser(input);
        if (parts.size() < 2) {
            ui->displayError("格式错误，请输入: 群名称 描述 [加群方式0/1/2] [public/private] [人数上限]");
            return;
        }
        // 校验可选的加群方式
        if (parts.size() > 2) {
            try {
                int jt = std::stoi(parts[2]);
                if (jt < 0 || jt > 2) {
                    ui->displayError("加群方式应为 0(直接加入)/1(需申请)/2(禁止加入)");
                    return;
                }
            } catch (...) {
                ui->displayError("无效的加群方式");
                return;
            }
        }
        current_input_buffer = input;
        ui->displaySystem("请输入成员ID (用空格分隔，至少2个): ");
    } else {
        auto info = CommandParser(current_input_buffer);
        std::string name = info[0];
        std::string desc = info[1];
        int join_type = info.size() > 2 ? std::stoi(info[2]) : 0;
        bool is_public = true;
        if (info.size() > 3) {
            std::string pub = info[3];
            is_public = !(pub == "private" || pub == "0");
        }
        int max_members = 500;
        if (info.size() > 4) {
            try {
                max_members = std::stoi(info[4]);
            } catch (...) {
                max_members = 500;
            }
        }

        std::vector<uint64_t> member_ids;
        auto parts = CommandParser(input);
        uint64_t my_uid = client->getUserId();
        for (const auto& p : parts) {
            try {
                uint64_t uid = std::stoull(p);
                if (uid != my_uid) {
                    member_ids.push_back(uid);
                }
            } catch (...) {
                ui->displayError("无效的成员ID: " + p);
                return;
            }
        }
        
        if (member_ids.size() < 2) {
            ui->displayError("至少需要2个成员");
            return;
        }
        
        ui->displaySystem("正在创建群组 " + name + "...");
        client->creatgroup(name, desc, is_public, join_type, member_ids, max_members);
        current_input_buffer.clear();
        current_state = MenuState::MAIN;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        showMainMenu();
    }
}

void commandHandle(const std::string& cmd) {
    if (cmd.empty()) return;

    // 特殊命令
    if (cmd == "/back" || cmd == "back" || cmd == "b") {
        if (client) client->cleanChatMod();
        if (ui) ui->setStatus("已退出聊天");
        current_state = MenuState::MAIN;
        showMainMenu();
        return;
    }
    
    if (cmd == "/clear" || cmd == "clear" || cmd == "cls") {
        ui->clearScreen();
        showMainMenu();
        return;
    }
    
    if (cmd == "/help" || cmd == "help" || cmd == "?") {
        showMainMenu();
        return;
    }

    if (cmd == "/quit" || cmd == "quit" || cmd == "exit" || cmd == "q") {
        if (client) client->disconnect();
        running = false;
        return;
    }

    // 聊天模式优先
    if (client && client->isInChatMode() && !cmd.empty() && cmd[0] != '/') {
        if(client->isInPrivate()) {
            client->sendprivateChat(client->getCurTarget(), cmd);
        }
        else if(client->isInGroupChat()) {
            client->sendGroupChat(client->getCurTarget(), cmd);
        }
        return;
    }

    // 状态机处理
    switch (current_state) {
        case MenuState::MAIN:
            processMainMenu(cmd);
            break;
        case MenuState::LOGIN:
            processLogin(cmd);
            break;
        case MenuState::REGISTER:
            processRegister(cmd);
            break;
        case MenuState::DELETE_ACCOUNT:
            processDeleteAccount(cmd);
            break;
        case MenuState::CHAT:
            processChat(cmd);
            break;
        case MenuState::SEND_FILE:
            processSendFile(cmd);
            break;
        case MenuState::SEND_GROUP_FILE:
            processSendGroupFile(cmd);
            break;
        case MenuState::DOWNLOAD_FILE:
            processDownloadFile(cmd);
            break;
        case MenuState::FRIEND_ADD:
            processFriendAdd(cmd);
            break;
        case MenuState::FRIEND_DELETE:
            processFriendDelete(cmd);
            break;
        case MenuState::FRIEND_PROCESS:
            processFriendProcess(cmd);
            break;
        case MenuState::GROUP_HISTORY:
            processGroupMembers(cmd);
            break;
        case MenuState::JOIN_GROUP:
            processJoinGroup(cmd);
            break;
        case MenuState::LEAVE_GROUP:
            processLeaveGroup(cmd);
            break;
        case MenuState::DISMISS_GROUP:
            processDismissGroup(cmd);
            break;
        case MenuState::KICK_MEMBER:
            processKickMember(cmd);
            break;
        case MenuState::SET_ADMIN:
            processSetAdmin(cmd);
            break;
        case MenuState::PENDING_REQUESTS:
            processPendingRequests(cmd);
            break;
        case MenuState::APPROVE_REQUEST:
            processApproveRequest(cmd);
            break;
        case MenuState::HISTORY:
            processHistory(cmd);
            break;
        case MenuState::RECALL:
            processRecall(cmd);
            break;
        case MenuState::GROUP_RECALL:
            processGroupRecall(cmd);
            break;
        case MenuState::BLOCK_USER:
            processBlock(cmd);
            break;
        case MenuState::CREATE_GROUP:
            processCreateGroup(cmd);
            break;
        case MenuState::MARK_READ:
            processMarkRead(cmd);
            break;
        case MenuState::MARK_GROUP_READ:
            processMarkGroupRead(cmd);
            break;
        default:
            ui->displayError("未知状态");
            current_state = MenuState::MAIN;
            showMainMenu();
            break;
    }
    
    // 处理待显示的消息
    std::deque<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        messages.swap(pending_messages);
    }
    while (!messages.empty()) {
        ui->displayMessage(messages.front());
        messages.pop_front();
    }
}
namespace fs = std::filesystem;
inline bool ensureDirectoryExists(const std::string& path) {
        try {
            if (fs::exists(path)) {
                return fs::is_directory(path);
            }
            bool created = fs::create_directories(path);
            if (created) {
                LOG_INFO << "Created directory: " << path;
            }
            return created;
        } catch (const std::exception& e) {
            LOG_ERROR << "Failed to create directory " << path << ": " << e.what();
            return false;
        }
    }

// ============ 主函数 ============
int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = 8000;
    bool use_tls = false;
    std::string cert_file, key_file;

    static struct option long_options[] = {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"tls", no_argument, 0, 't'},
        {"cert", required_argument, 0, 'c'},
        {"key", required_argument, 0, 'k'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:t:c:k:", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = std::stoi(optarg);
                break;
            case 't':
                use_tls = true;
                break;
            case 'c':
                cert_file = optarg;
                break;
            case 'k':
                key_file = optarg;
                break;
            default:
                std::cerr << "Usage: " << argv[0] << " -h <host> -p <port> [-t] [--cert cert.pem] [--key key.pem]" << std::endl;
                return 1;
        }
    }

    

    if (!ensureDirectoryExists("./Clogs")) {
        std::cerr << "Failed to create Logs directory" << std::endl;
        return 1;
    }
    LOGinit("chat_server", "./Clogs", false);

    ui = std::make_unique<cUI>();
    if (!ui->UIinit()) {
        std::cerr << "UI init failed" << std::endl;
        return 1;
    }

    client = std::make_unique<ChatClient>();
    client->setMessageCallback([&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(pending_mutex);
        pending_messages.push_back(msg);
    });

    if (!client->connect(host, port, use_tls, cert_file, key_file)) {
        ui->displayError("服务器链接失败，请检查");
        ui->run([](const std::string&) {}, [] {
            std::deque<std::string> messages;
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                messages.swap(pending_messages);
            }
            while (!messages.empty()) {
                ui->displayMessage(messages.front());
                messages.pop_front();
            }
        });
        return 1;
    }

    ui->displaySystem("已连接到 " + host + ":" + std::to_string(port));
    if (use_tls) {
        ui->displaySystem("TLS加密已启动");
    }

    showMainMenu();

    ui->run(commandHandle, [] {
        std::deque<std::string> messages;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            messages.swap(pending_messages);
        }
        while (!messages.empty()) {
            ui->displayMessage(messages.front());
            messages.pop_front();
        }
    });
    
    client->disconnect();
    return 0;
}