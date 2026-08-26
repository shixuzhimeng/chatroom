# ChatRoom聊天室项目
基于C++实现的TCP网络聊天室，支持多种业务操作，如聊天（私聊，群聊），文件传输，好友管理，群组管理等。

## 项目简介
这个项目是一个完整的聊天室项目，采用主从Reactor模型，protobuf完成消息的序列化和反序列化，Mysql数据持久化存储，也可进行TLS通信加密，支持高并发链接。
系统包括帐户管理、好友管理、群组管理、文件操作、消息收发等核心功能。

## 主要特性
- **网络模型**：基于epoll的主从Reactor模型，支持高并发
- **通信协议**：protobuf序列化和反序列化
- **数据存储**：MySQL+连接池，支持事务操作
- **传输通信**：支持TLS通信加密（单向加密）
- **消息管理**：私聊，群聊，离线消息，撤回消息，会话列表
- **文件传输**：支持断点续传，文件MD5校验，离线文件
- **好友系统**：好友添加/删除/拉黑，查看好友列表
- **群组系统**：群组的创建/解散，成员管理，群组权限设置

## 环境要求
- **语言**：C++17
- **序列化**：Google Protobuf 3.21+
- **数据库**：MySQL 8.0+
- **网络库**：Linux下原生epoll
- **安全**：OpenSSL 1.1.1+
- **日志**：Google Glog
- **配置**：JSON
- **UI**： ncurses

## 项目目录结构
```
ChatRoom/
├── account/           # 账号管理模块
│   ├── Account.h      # 认证处理器
│   ├── HashSalt.h     # 密码加密与盐值
│   ├── Manager.h      # 会话管理
│   └── yanzheng.h     # 验证码管理
├── chat/              # 聊天模块
│   ├── chathandle.h   # 私聊处理器
│   └── groupmessagehandle.h  # 群聊处理器
├── file/              # 文件模块
│   ├── fileHandle.h   # 文件传输处理器
│   ├── fileManage.h   # 文件管理 (存储/定期清理)
│   └── md5.h          # MD5工具校验
├── friend/            # 好友模块
│   ├── FriendHandle.h # 好友关系处理器
│   └── OnlineManager.h # 在线状态管理
├── group/             # 群组模块
│   └── grouphandle.h  # 群组处理器
├── mysql/             # 数据库模块
│   ├── baseDAO.h      # 数据库基类
│   ├── mysqlPool.h    # 连接池
│   ├── userDAO.h      # 用户DAO
│   ├── friendDAO.h    # 好友DAO
│   ├── groupDAO.h     # 群组DAO
│   ├── messageDAO.h   # 消息DAO
│   ├── groupmessageDAO.h # 群消息DAO
│   ├── fileDAO.h      # 文件DAO
│   └── pingbiDAO.h    # 屏蔽DAO
├── net/               # 网络模块
│   ├── epoll.h        # Epoll封装 & TCP连接（可选）
│   ├── reactor.h      # Reactor模式实现（主从）
│   └── thread_pool.h  # 线程池
│   └── chat_server.h  # 服务端主类
│   └── main.cpp       # 服务端入口
├── protobuf/          # Protobuf定义
│   ├── p.proto        # 网络协议定义
│   ├── mysql.proto    # 数据库序列化定义
│   ├── p.h            # 编解码器
│   └── mysql_p.h      # 序列化工具
├── TLS/               # TLS模块
│   ├── TLS.h          # 服务端TLS
├── tool/              # 工具模块
│   ├── logging.h      # 日志封装
│   ├── tool.h         # 通用工具
│   ├── Check.h        # 输入验证
│   ├── deduplicator.h # 消息去重
│   └── limiter.h      # 频率限制
├── JSON/              # 配置模块
│   ├── Config.h       # 配置管理
│   └── Config.json    # 配置文件
├── client/            # 客户端
│   ├──  chat_client.h # 客户端主类
│   ├──  cmain.cpp     # 客户端入口
│   ├──  ui.h          # 客户端UI (ncurses)（两版）
│   └── TLSclient.h    # 客户端TLS
```

## 客户端的使用命令 (注： `[]` 为可选选项)（较为麻烦的一版）(cui这一版)
### 用户帐号相关
/register <用户名> <密码> <邮箱> [昵称]	   注册新用户	  /register john 123456 john@email.com hhh
/login <用户名> <密码> [设备ID]	           登录账号	     /login john 123456
/logout	                                登出当前账号   /logout
/deleteaccount [密码]	                 删除账号	   /deleteaccount 123456

### 私聊功能
/chat <用户ID> <消息>	                  发送私聊消息	 /chat 1001 你好，最近怎么样？（使用这条命令之后就可以一直发送消息了  /back退出）
/history <用户ID> [条数]	              查看私聊历史	 /history 1001 20
/recall <消息ID>	                     撤回私聊消息	/recall 12345
/read [用户ID]	                         标记消息已读	/read 1001
/back                                  退出当前用户聊天 /back

### 群聊功能
/creategroup <群名> <描述> [public/private] [人数限制]	创建群组	   /creategroup 技术交流 技术讨论群 public 100
/joingroup <群ID> [验证消息]	                       加入群组	      /joingroup 5001 我想加入学习
/leavegroup <群ID>	                                  退出群组	     /leavegroup 5001
/dismissgroup <群ID>	                          解散群组（仅群主）  /dismissgroup 5001
/groupchat <群ID> <消息>	                         发送群消息	      /groupchat 5001 大家好！（使用这条命令之后就可以一直发送消息了  /back退出）
/grouphistory <群ID> [条数]	                         查看群聊历史     /grouphistory 5001 30
/grouprecall <消息ID>	                             撤回群消息	     /grouprecall 12345
/groupread <群ID>	                               标记群消息已读     /groupread 5001
/back                                             退出当前用户聊天    /back

### 群管理功能
/setadmin <群ID> <用户ID> <true/false>	         设置/取消管理员	/setadmin 5001 1001 true
/kick <群ID> <用户ID>	                            踢出成员	   /kick 5001 1001
/pending <群ID>	                                 查看待审批请求	   /pending 5001
/approve <请求ID> <true/false>	                   审批入群请求	   /approve 1001 true
/groups	                                          查看群组列表	   /groups
/groupmembers <群ID>	                          查看群成员列表	/groupmembers 5001

### 好友功能
/friendlist	                                      查看好友列表	  /friendlist
/friendadd <用户ID> [验证消息]	                    发送好友请求	/friendadd 1001 我是张三
/frienddel <用户ID>	                                删除好友	  /frienddel 1001
/friendprocess <请求ID> <true/false>	           处理好友请求	   /friendprocess 1001 true

### 文件传输功能
/sendfile <用户ID> <文件路径> [附言]	            发送文件给好友	/sendfile 1001 ./test.pdf 请查收
/sendgroupfile <群ID> <文件路径> [附言]	           发送文件到群组	/sendgroupfile 5001 ./wenjian.md 分享
/download <文件ID>	                                下载文件	 /download abc123
/offline_files	                                  查看离线文件	 /offline_files

### 屏蔽功能
/block <用户ID>	                拉黑用户	/block 1001
/unblock <用户ID>	            取消拉黑	/unblock 1001
/blocklist	                   查看黑名单	/blocklist

### 其他命令
exit 或 /quit	     退出程序	 exit
/clear  或  clear    清屏       clear