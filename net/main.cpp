#include "chat_server.h"
#include "JSON/Config.h"
#include "tool/logging.h"
#include "mysql/mysqlPool.h"
#include <iostream>
#include <signal.h>
#include <getopt.h>
#include <cstdlib>
#include "TLS/TLS.h"

std::unique_ptr<ChatServer> g_server;

static std::string envOr(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : def;
}

void signalHander(int  sig) {
    LOG_INFO << "Received signal " << sig << ", shutting down... ";
    if(g_server) {
        g_server->stop();
    }
    exit(0);
}

int main(int argc, char* argv[]) {
    srand(static_cast<unsigned>(time(nullptr)));

    std::string host = "127.0.0.1";
    uint16_t port = 8000;
    std::string config_path = "JSON/Config.json";
    int sub_reactor = 4;
    int threads = 8;

    static struct option options[] = 
    {
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"config", required_argument, 0, 'c'},
        {"sub", required_argument, 0, 's'},
        {"thread", required_argument, 0, 't'},
        {0,0,0,0}  // 结束标志
    };
    
    int opt;
    while((opt = getopt_long(argc, argv, "h:p:c:s:t:", options, nullptr)) != -1) {
        switch (opt)
        {
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = static_cast<uint16_t>(std::stoi(optarg));
            break;
        case 'c':
            config_path = optarg;
            break;
        case 's':
            sub_reactor = std::stoi(optarg);
            break;
        case 't':
            threads = std::stoi(optarg);
            break;
        default:
            break;
        }
    }


    // 交互界面
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║                       ChatRoom                       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

    std::cout << "Choose:";
    std::cout << "Host:          " << host << "\n";
    std::cout << "Port:          " << port << "\n";
    std::cout << "Config_path    " << config_path << "\n";
    std::cout << "Sub_reactor    " << sub_reactor << "\n";
    std::cout << "Threads        " << threads << "\n";


    // 加载配置
    if(!LOAD_CONFIG(config_path)) {
        std::cerr << "Failed to load config: " << config_path << "\n";
        return 1;
    }

    auto&& server_config = Config::getInstance().getJson("server");
    if (!server_config.contains("host") || host != "0.0.0.0") {
        Config::getInstance().set("server.host", host);
    }

    // 初始化日志
    LOGinit("chat_server", "./Logs", false);

    LOG_INFO << "Chat Server Start";
    LOG_INFO << "host : " << host << ", port : " << port;
    LOG_INFO << "Config file: " << config_path;
    LOG_INFO << "Sub_reactor: " << sub_reactor;
    LOG_INFO << "threads: " << threads;

    // 加载证书（必须在 LOAD_CONFIG 之后读取，否则配置尚未加载）
    std::string cert_file = GET_CONFIG("tls.cert_file", std::string(""));
    std::string key_file = GET_CONFIG("tls.key_file", std::string(""));
    bool enable_tls = GET_CONFIG("tls.enabled", false) &&
                      !cert_file.empty() && !key_file.empty();

    TLSContext tls_ctx;
    if(enable_tls) {
        if(!tls_ctx.init(cert_file, key_file)) {
            LOG_FATAL << "Failed to initialize TLS";
            return 1;
        }
        LOG_INFO << "TLS enabled";
    } else {
        LOG_INFO << "TLS disabled";
    }
    
    // 初始化数据库
    auto&& db_config = Config::getInstance().getJson("database");
    auto& pool = ConnectionPool::getInstance();

    std::string db_host = envOr("DB_HOST", db_config.value("host", "localhost"));
    uint16_t db_port = static_cast<uint16_t>(std::stoi(envOr("DB_PORT", std::to_string(db_config.value("port", 3306)))));
    std::string db_user = envOr("DB_USER", db_config.value("user", "root"));
    std::string db_password = envOr("DB_PASSWORD", db_config.value("password", ""));
    std::string db_database = envOr("DB_NAME", db_config.value("database", "test"));
    size_t pool_min = db_config.value("pool_min", 5);
    size_t pool_max = db_config.value("pool_max", 20);

    if (!pool.init(db_host, db_port, db_user, db_password, db_database, pool_min, pool_max)) {
        LOG_FATAL << "Database connection pool init failed";
        return 1;
    }
    LOG_INFO << "Database connection pool initialized";


    // 测试数据库连接
    UserDAO user_dao;
    USER test_user;
    if(user_dao.getUserByUsername("admin", test_user)) {
        LOG_INFO << "Database test: found admin user (ID: " << test_user.user_id << ")";
    }
    else {
        LOG_WARN << "Database test: admin user not found (normal on a fresh DB)";
    }

    g_server = std::make_unique<ChatServer>(host, port, sub_reactor, enable_tls ? tls_ctx.get() : nullptr);

    signal(SIGINT, signalHander);
    signal(SIGTERM, signalHander);
    signal(SIGPIPE, SIG_IGN);

    //g_server->init();
    g_server->start();

    LOG_INFO << " Chat server started ";
    std::cout << "Server is running on :" << host << ":" << port << "\n";
    std::cout << "Press Ctrl + C to stop server";

    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}