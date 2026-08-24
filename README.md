# ChatRoom聊天室项目
基于C++实现的TCP网络聊天室，采用主从Reactor模型，protobuf完成消息的序列化和反序列化，Mysql存储数据

## 客户端的使用命令 (注： `[]` 为可选选项)
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