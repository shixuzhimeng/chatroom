#include "chat_server.h"
#include "JSON/Config.h"
#include "logging.h"
#include "mysql/mysqlPool.h"
#include <iostream>
#include <signal.h>
#include <getopt.h>

std::unique_ptr<ChatServer> g_server;

