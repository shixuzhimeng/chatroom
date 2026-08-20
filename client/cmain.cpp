#include "chat_client.h"
#include "ui.h"
#include "../logging.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <getopt.h>
#include <csignal>

static std::unique_ptr<ChatClient> client;
static std::unique_ptr<cUI> ui;
static std::atomic<bool> running(true);

static std::vector<std::string> CommandParser(const std::string& cmd) {
    std::vector<std::string> parts;
    std::stringstream ss(cmd);
    std::string part;

    while(ss >> part) {
        parts.push_back(part);
    }

    return parts;
}

void commandHandle(const std::string& cmd) {
    if(cmd == "/quit" || cmd == "exit") {
        if(client) {
            client->disconnect();
        }
        running = false;
        return ;
    }
    else if(cmd == "/clear" || cmd == "clear") {
        ui->clearScreen();
        ui->displaySystem("清空");
    }

    auto parts = CommandParser(cmd);
    if(parts.empty()) {
        return ;
    }

    try {
        if(parts[0] == "/login" && parts.size() >= 3) {
            std::string device_id = parts.size() > 3 ? parts[3] : "client_" + tool::randString(8);
            client->Login(parts[1], parts[2], device_id);
        }
        else if(parts[0] == "/register" && parts.size() >= 4) {
            std::string nick = parts.size() > 4 ? parts[4] : parts[1];
            nick = tool::trim(nick);
            LOG_DEBUG << "register : username:" << parts[1] << ", password:" << parts[2] << ", email:" << parts[3] << ", nickname:" << parts[4];
            client->registerUser(parts[1], parts[2], parts[3], nick);
        }
        else if(parts[0] == "/logout") {
            client->Logout();
            ui->displaySystem("已登出");
        }
        else if(parts[0] == "/deleteaccount") {
            std::string password = (parts.size() > 1) ? parts[1] : "";
            ui->displaySystem("删除账户");
            client->deleteUser(password);
        }
        else if(parts[0] == "/chat" && parts.size() >= 3) {
            uint64_t uid = std::stoull(parts[1]);
            std::string msg = cmd.substr(cmd.find(parts[2]));
            client->sendprivateChat(uid, msg);
        }
        else if(parts[0] == "/groupchat" && parts.size() >= 3) {
            uint64_t gid = std::stoull(parts[1]);
            std::string msg = cmd.substr(cmd.find(parts[2]));
            client->sendGroupChat(gid, msg);
        }
        else if(parts[0] == "/history" && parts.size() >= 2) {
            uint64_t uid = std::stoull(parts[1]);
            int limit = parts.size() > 2 ? std::stoi(parts[2]) : 50;
            client->getHistory(uid, limit);
        }
        else if(parts[0] == "/grouphistory" && parts.size() >= 2) {
            uint64_t gid = std::stoull(parts[1]);
            int limit = parts.size() > 2 ? std::stoi(parts[2]) : 50;
            client->getGroupHistory(gid, limit);
        }
        else if(parts[0] == "/recall" && parts.size() >= 2) {
            client->recallMessage(std::stoull(parts[1]));
        }
        else if(parts[0] == "/grouprecall" && parts.size() >= 2) {
            client->recallGroupMessage(std::stoull(parts[1]));
        }
        else if(parts[0] == "/read") {
            uint64_t uid = parts.size() > 1 ? std::stoull(parts[1]) : 0;
            client->markRead(uid);
        }
        else if(parts[0] == "/groupread" && parts.size() >= 2) {
            client->markGroupRead(std::stoull(parts[1]));
        }
        else if(parts[0] == "/groups") {
            client->getGroupList();
        }
        else if(parts[0] == "/groupmembers" && parts.size() >= 2) {
            client->getGroupMembers(std::stoull(parts[1]));
        }
        else if(parts[0] == "/creategroup" && parts.size() >= 4) {
            std::string name = parts[1];
            std::string desc = parts[2];

            bool is_public = true;
            int join_type = 0;
            std::vector<uint64_t> member_ids;
            int idx = 3;

            if(parts.size() > idx && (parts[idx] == "public" || parts[idx] == "private")) {
                is_public = (parts[idx] == "public");
                idx++;
            }

            if(parts.size() > idx) {
                try {
                    join_type = std::stoi(parts[idx]);
                    idx++;
                }
                catch(...) {

                }
            }

            for(size_t i = idx; i < parts.size(); ++i) {
                try {
                    uint64_t uid = std::stoull(parts[i]);
                    member_ids.push_back(uid);
                }
                catch(const std::exception& e) {
                    ui->displayError("无效的成员ID： " + parts[i]);
                    return ;
                }
            }

            if(member_ids.size() < 2) {
                ui->displayError("人数少于三人，创建群聊");
                ui->displayError("除自己外，至少添加两个用户");
                return ;
            }

            client->creatgroup(name, desc, is_public, join_type, member_ids);
        }
        else if(parts[0] == "/joingroup" && parts.size() >= 2) {
            std::string msg = parts.size() > 2 ? parts[2] : "";
            client->joinGroup(std::stoull(parts[1]), msg);
        }
        else if(parts[0] == "/leavegroup" && parts.size() >= 2) {
            client->leaveGroup(std::stoull(parts[1]));
        }
        else if(parts[0] == "/dismissgroup" && parts.size() >= 2) {
            client->dismissGroup(std::stoull(parts[1]));
        }
        else if(parts[0] == "/setadmin" && parts.size() >= 4) {
            client->setAdmin(std::stoull(parts[1]), std::stoull(parts[2]), parts[3] == "true");
        }
        else if(parts[0] == "/kick" && parts.size() >= 3) {
            client->kickMember(std::stoull(parts[1]), std::stoull(parts[2]));
        }
        else if(parts[0] == "/pending" && parts.size() >= 2) {
            client->getPendingRequests(std::stoull(parts[1]));
        }
        else if(parts[0] == "/approve" && parts.size() >= 3) {
            client->processJoinRequest(std::stoull(parts[1]), parts[2] == "true");
        }
        else if(parts[0] == "/friendlist") {
            client->getFriendList();
        }
        else if(parts[0] == "/friendadd" && parts.size() >= 2) {
            std::string msg = parts.size() > 2 ? parts[2] : "";
            client->sendFriendRequest(std::stoull(parts[1]), msg);
        }
        else if(parts[0] == "/frienddel" && parts.size() >= 2) {
            client->deleteFriend(std::stoull(parts[1]));
        }
        else if(parts[0] == "/friendprocess" && parts.size() >= 3) {
            client->processFriendRequest(std::stoull(parts[1]), parts[2] == "true");
        }
        else if(parts[0] == "/block" && parts.size() >= 2) {
            client->blockUser(std::stoull(parts[1]));
        }
        else if(parts[0] == "/unblock" && parts.size() >= 2) {
            client->unblockUser(std::stoull(parts[1]));
        }
        else if(parts[0] == "/blocklist") {
            client->getBlockList();
        }
        else if (parts[0] == "/sendfile" && parts.size() >= 3) {
            uint64_t to_uid = std::stoull(parts[1]);
            std::string file_path = parts[2];
            std::string text = parts.size() > 3 ? parts[3] : "";
            client->sendFile(to_uid, file_path, text);
        }
        else if (parts[0] == "/sendgroupfile" && parts.size() >= 3) {
            uint64_t group_id = std::stoull(parts[1]);
            std::string file_path = parts[2];
            std::string text = parts.size() > 3 ? parts[3] : "";
            client->sendGroupFile(group_id, file_path, text);
        }
        else if(parts[0] == "/download" && parts.size() >= 2) {
            client->downloadFile(parts[1]);
        }
        else if(parts[0] == "/offline_files") {
            client->getOfflineFiles();
        } 
        else {
            ui->displayError("未知命令");
        }
    }
    catch (const std::exception& e) {
        ui->displayError("参数错误: " + std::string(e.what()));
    }
}

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
    while((opt = getopt_long(argc, argv, "h:p:t:c:k:", long_options, nullptr)) != -1) {
        switch(opt) {
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
                std::cerr << "Usage: " << argv[0] << "-h <host> -p <port> [-t] [--cert cert.pem] [--key key.pem]" << std::endl;
                return 1;
        }
    }

    LOGinit("chat_client", "./Clogs", false);

    ui = std::make_unique<cUI>();
    if(!ui->UIinit()) {
        std::cerr << "UI init failed" << std::endl;
        return 1;
    }

    client = std::make_unique<ChatClient>();
    client->setMessageCallback([&](const std::string& msg) {
        ui->displayMessage(msg);
    });

    if(!client->connect(host, port, use_tls, cert_file, key_file)) {
        ui->displayError("服务器链接失败，请检查");
        ui->run([](const std::string&) {});
        return 1;
    }

    ui->displaySystem("已连接到 " + host + ":" + std::to_string(port));
    if(use_tls) {
        ui->displaySystem("TLS加密已启动");
    }

    ui->run(commandHandle);
    client->disconnect();
    return 0;
}